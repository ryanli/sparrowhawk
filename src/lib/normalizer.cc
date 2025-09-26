// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Copyright 2015 and onwards Google, Inc.
#include "sparrowhawk/normalizer.h"

#include <google/protobuf/text_format.h>

#include <filesystem>
#include <memory>
#include <string>

#include "sparrowhawk/io_utils.h"
#include "sparrowhawk/protobuf_parser.h"
#include "sparrowhawk/protobuf_serializer.h"
#include "sparrowhawk/sentence_boundary.h"
#include "sparrowhawk/spec_serializer.h"
#include "sparrowhawk/string_utils.h"
#include "src/proto/items.pb.h"
#include "src/proto/serialization_spec.pb.h"
#include "src/proto/sparrowhawk_configuration.pb.h"

namespace speech::sparrowhawk {
namespace {

std::string DebugString(const google::protobuf::Message &message) {
  google::protobuf::TextFormat::Printer printer;
  printer.SetSingleLineMode(true);
  printer.SetUseUtf8StringEscaping(true);
  printer.SetUseShortRepeatedPrimitives(true);
  std::string out;
  if (!printer.PrintToString(message, &out)) {
    LOG(WARNING) << "Error converting message to text format.";
  }
  return out;
}

}  // namespace

// TODO(rws): We actually need to do something with this.
const char kDefaultSentenceBoundaryRegexp[] = "[\\.:!\\?] ";

Normalizer::Normalizer() {}

Normalizer::~Normalizer() {}

bool Normalizer::Setup(const std::string &configuration_proto,
                       const std::string &pathname_prefix) {
  SparrowhawkConfiguration configuration;
  std::string proto_string = IOStream::LoadFileToString(
      std::filesystem::path(pathname_prefix) / configuration_proto);
  if (!google::protobuf::TextFormat::ParseFromString(proto_string,
                                                     &configuration))
    return false;
  if (!configuration.has_tokenizer_grammar())
    LOG(ERROR)
        << "Configuration does not define a tokenizer-classifier grammar";
  if (!configuration.has_verbalizer_grammar())
    LOG(ERROR) << "Configuration does not define a verbalizer grammar";
  tokenizer_classifier_rules_.reset(new RuleSystem);
  if (!tokenizer_classifier_rules_->LoadGrammar(
          configuration.tokenizer_grammar(), pathname_prefix))
    return false;
  verbalizer_rules_.reset(new RuleSystem);
  if (!verbalizer_rules_->LoadGrammar(configuration.verbalizer_grammar(),
                                      pathname_prefix))
    return false;
  std::string sentence_boundary_regexp;
  if (configuration.has_sentence_boundary_regexp()) {
    sentence_boundary_regexp = configuration.sentence_boundary_regexp();
  } else {
    sentence_boundary_regexp = kDefaultSentenceBoundaryRegexp;
  }
  sentence_boundary_.reset(new SentenceBoundary(sentence_boundary_regexp));
  if (configuration.has_sentence_boundary_exceptions_file()) {
    if (!sentence_boundary_->LoadSentenceBoundaryExceptions(
            std::filesystem::path(pathname_prefix) /
            configuration.sentence_boundary_exceptions_file())) {
      LOG(ERROR) << "Cannot load sentence boundary exceptions file: "
                 << configuration.sentence_boundary_exceptions_file();
    }
  }
  if (configuration.has_serialization_spec()) {
    std::string spec_string =
        IOStream::LoadFileToString(std::filesystem::path(pathname_prefix) /
                                   configuration.serialization_spec());
    SerializeSpec spec;
    if (spec_string.empty() ||
        !google::protobuf::TextFormat::ParseFromString(spec_string, &spec) ||
        (spec_serializer_ = Serializer::Create(spec)) == nullptr) {
      LOG(ERROR) << "Failed to load a valid serialization spec from file: "
                 << configuration.serialization_spec();
      return false;
    }
  }
  return true;
}

bool Normalizer::Normalize(const std::string &input,
                           std::string *output) const {
  std::unique_ptr<Utterance> utt;
  utt.reset(new Utterance);
  if (!Normalize(utt.get(), input)) return false;
  *output = LinearizeWords(utt.get());
  return true;
}

bool Normalizer::Normalize(Utterance *utt, const std::string &input) const {
  return TokenizeAndClassifyUtt(utt, input) && VerbalizeUtt(utt);
}

bool Normalizer::NormalizeAndShowLinks(const std::string &input,
                                       std::string *output) const {
  std::unique_ptr<Utterance> utt;
  utt.reset(new Utterance);
  if (!Normalize(utt.get(), input)) return false;
  *output = ShowLinks(utt.get());
  return true;
}

bool Normalizer::TokenizeAndClassifyUtt(Utterance *utt,
                                        const std::string &input) const {
  typedef fst::StringCompiler<fst::StdArc> Compiler;
  Compiler compiler(fst::TokenType::BYTE);
  MutableTransducer input_fst, output;
  if (!compiler(input, &input_fst)) {
    LOG(ERROR) << "Failed to compile input string \"" << input << "\"";
    return false;
  }
  if (!tokenizer_classifier_rules_->ApplyRules(input_fst, &output,
                                               true /*  use_lookahead */)) {
    LOG(ERROR) << "Failed to tokenize \"" << input << "\"";
    return false;
  }
  MutableTransducer shortest_path;
  fst::ShortestPath(output, &shortest_path);
  ProtobufParser parser(&shortest_path);
  if (!parser.ParseTokensFromFST(utt, true /* set SEMIOTIC_CLASS */)) {
    LOG(ERROR) << "Failed to parse tokens from FST for \"" << input << "\"";
    return false;
  }
  return true;
}

// As in Kestrel's Run(), this processes each token in turn and creates the Word
// stream, adding words each with a unique wordid.  Takes a different action on
// the type:
//
// PUNCT: do nothing
// SEMIOTIC_CLASS: call verbalizer FSTs
// WORD: add to word stream
bool Normalizer::VerbalizeUtt(Utterance *utt) const {
  for (int i = 0; i < utt->linguistic().tokens_size(); ++i) {
    Token *token = utt->mutable_linguistic()->mutable_tokens(i);
    std::string token_form = DebugString(*token);
    token->set_first_daughter(-1);  // Sets to default unset.
    token->set_last_daughter(-1);   // Sets to default unset.
    // Add a single silence for punctuation that forms phrase breaks. This is
    // set via the grammar, though ultimately we'd like a proper phrasing
    // module.
    if (token->type() == Token::PUNCT) {
      if (token->phrase_break() &&
          (utt->linguistic().words_size() == 0 ||
           utt->linguistic().words(utt->linguistic().words_size() - 1).id() !=
               "sil")) {
        AddWord(utt, token, "sil");
      }
    } else if (token->type() == Token::SEMIOTIC_CLASS) {
      if (!token->skip()) {
        LOG(DEBUG) << "Verbalizing: [" << token_form << "]";
        std::string words;
        if (VerbalizeSemioticClass(*token, &words)) {
          AddWords(utt, token, words);
        } else {
          LOG(WARNING) << "First-pass verbalization FAILED for [" << token_form
                       << "]";
          // Back off to verbatim reading
          std::string original_token = token->name();
          token->Clear();
          token->set_name(original_token);
          token->set_verbatim(original_token);
          if (VerbalizeSemioticClass(*token, &words)) {
            LOG(WARNING) << "Reversion to verbatim succeeded for ["
                         << original_token << "]";
            AddWords(utt, token, words);
          } else {
            // If we've done our checks right, we should never get here
            LOG(ERROR) << "Verbalization FAILED for [" << token_form
                       << "], falling back to token name";
            AddWords(utt, token, token->name());
          }
        }
      }
    } else if (token->type() == Token::WORD) {
      if (token->has_wordid()) {
        AddWord(utt, token, token->wordid());
      } else {
        LOG(ERROR) << "Token [" << token_form
                   << "] has type WORD but there is no word id";
      }
    } else {
      LOG(ERROR) << "No type found for [" << token_form << "]";
    }
  }
  LOG(DEBUG) << "Verbalize output: Words\n" << LinearizeWords(utt) << "\n";
  return true;
}

bool Normalizer::VerbalizeSemioticClass(const Token &markup,
                                        std::string *words) const {
  Token local(markup);
  CleanFields(&local);
  MutableTransducer input_fst;
  if (spec_serializer_ == nullptr) {
    ProtobufSerializer serializer(&local, &input_fst);
    serializer.SerializeToFst();
  } else {
    input_fst = spec_serializer_->Serialize(local);
  }
  if (!verbalizer_rules_->ApplyRules(input_fst, words,
                                     false /* use_lookahead */)) {
    LOG(ERROR) << "Failed to verbalize \"" << DebugString(local) << "\"";
    return false;
  }
  return true;
}

std::vector<string> Normalizer::SentenceSplitter(
    const std::string &input) const {
  return sentence_boundary_->ExtractSentences(input);
}

}  // namespace speech::sparrowhawk
