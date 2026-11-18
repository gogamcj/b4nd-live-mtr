# Basic Pitch model (Spotify)

`nmp.onnx` is the ONNX export of the Basic Pitch "icassp_2022" model, copied unchanged from the
`basic-pitch` 0.4.0 Python package (https://github.com/spotify/basic-pitch).

Licensed under the Apache License 2.0 (see `LICENSE`). Copyright 2022 Spotify AB.

The B4ND analyzer runs it with onnxruntime (`b4nd_analyzer/transcribe.py`). The windowing and the
note decoding there follow `basic_pitch/inference.py` and `basic_pitch/note_creation.py`
(Apache-2.0), rewritten without librosa / pretty_midi / tensorflow.
Basic Pitch
Copyright 2022 Spotify AB

This product includes software developed at
Spotify AB (http://www.spotify.com/).

This product includes software from Librosa (ISC).
* Copyright (C) 2013--2017, librosa development team.

This product includes software from mir_eval (MIT)
* Copyright (C) 2014 Colin Raffel

This product includes software from numpy (BSD)
* Copyright (C) 2005-2022, NumPy Developers.

This product includes software from pretty-midi (MIT)
* Copyright (C) 2014 Colin Raffel

This product includes software from resampy (ISC)
* Copyright (C) 2016, Brian McFee

This product includes software from scipy (BSD)
* Copyright (C) 2001-2002 Enthought, Inc. 2003-2022, SciPy Developers

This product includes software from tensorflow (Apache 2.0)
* Copyright (C) 2019 Google, LLC <packages@tensorflow.org>

The tests for `basic-pitch` include audio files from the
Vocadito dataset liscened under Creative Commons
Attribution 4.0 International. The dataset can be found at:
https://zenodo.org/record/5578807#.YnRm5vPMKDU
