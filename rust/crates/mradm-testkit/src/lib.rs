//! Test-only support shared by the Rust tests and the consistency tools: an ADM XML author
//! ([`adm`]), a RIFF / PCM author ([`wave`]) and the deterministic fixtures of the consistency
//! matrix ([`fixtures`]). Both authors are independent of the production crates, so a fixture
//! cannot inherit a bug of the code it tests. Ported from `tests/support/adm_fixture.h`,
//! `tests/support/wav_fixture.h` and `tests/tools/make_fixture.cpp` (R3 A), byte for byte.
#![forbid(unsafe_code)]

pub mod adm;
pub mod fixtures;
pub mod wave;
