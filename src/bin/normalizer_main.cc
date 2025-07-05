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
// Very simple stand-alone binary to run Sparrowhawk normalizer on a line of
// text.
//
// It runs the sentence boundary detector on the input, and then normalizes each
// sentence.
//
// As an example of use, build the test data here, and put them somewhere, such
// as /tmp/sparrowhawk_af.textproto.
//
// Then copy the relevant fars and protos there, edit the protos and then run:
//
// bazel run //:normalizer -- --config=/tmp/sparrowhawk_af.textproto
//
// Then input a few sentences on one line such as:
//
// Kameelperde het 'n kenmerkende voorkoms, met hul lang nekke en relatief
// kort lywe. Hulle word 4,3 - 5,7m lank. Die bulle is effens langer as die
// koeie.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "sparrowhawk/normalizer.h"

ABSL_FLAG(bool, multi_line_text, false,
          "Text is spread across multiple lines.");
ABSL_FLAG(std::string, config, "", "Path to the configuration proto.");
ABSL_FLAG(std::string, path_prefix, "./",
          "Optional path prefix if not relative.");

void NormalizeInput(const std::string &input,
                    speech::sparrowhawk::Normalizer *normalizer) {
  const std::vector<std::string> sentences =
      normalizer->SentenceSplitter(input);
  for (const auto &sentence : sentences) {
    std::string output;
    normalizer->Normalize(sentence, &output);
    std::cout << output << std::endl;
  }
}

int main(int argc, char **argv) {
  absl::InitializeLog();
  absl::ParseCommandLine(argc, argv);

  using speech::sparrowhawk::Normalizer;
  std::set_new_handler(FailedNewHandler);
  std::unique_ptr<Normalizer> normalizer;
  normalizer.reset(new Normalizer());
  CHECK(normalizer->Setup(absl::GetFlag(FLAGS_config),
                          absl::GetFlag(FLAGS_path_prefix)));
  std::string input;
  if (absl::GetFlag(FLAGS_multi_line_text)) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!input.empty()) input += " ";
      input += line;
    }
    NormalizeInput(input, normalizer.get());
  } else {
    while (std::getline(std::cin, input)) {
      NormalizeInput(input, normalizer.get());
    }
  }
  return 0;
}
