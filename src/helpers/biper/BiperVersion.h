// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
//
// The layer's version, as the boot banner prints it. The product version
// lives in biper/VERSION (release.sh names the image after it); this header
// must repeat it, because the compiler cannot read that file. release.sh
// refuses to build an image whose banner would disagree with the file name —
// v0.8 shipped printing "v0.7" because the banner was a forgotten literal.
#pragma once
#define BIPER_LAYER_VERSION "0.8.19"
