# Sparrowhawk

Sparrowhawk is an open-source implementation of Google's Kestrel text-to-speech
text normalization system.  It follows the discussion of the Kestrel system as
described in:

Ebden, Peter and Sproat, Richard. 2015. The Kestrel TTS text normalization
system. Natural Language Engineering, Issue 03, pp 333-353.

After sentence segmentation (`sentence_boundary.h`), the individual sentences are
first tokenized with each token being classified, and then passed to the
normalizer. The system can output as an unannotated string of words, and richer
annotation with links between input tokens, their input string positions, and
the output words is also available.

## Usage

This version is a fork of the original
[`google/sparrowhawk`](https://github.com/google/sparrowhawk) package, which
was archived in 2022.

Instead of using autotools for building, this version uses
[Bazel](https://bazel.build) for dependency management and building.

Tested on Bazel 8.2.1, but should in general work with any version that has
Bzlmod support.
