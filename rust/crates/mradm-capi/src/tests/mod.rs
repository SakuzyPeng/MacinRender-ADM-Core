//! The C ABI checks ported from the C++ consumer tests (R3 E): each calls the `extern "C"` exports
//! directly, as a C caller would. Unit tests link without the C++ islands, so nothing here reaches
//! an encoder, an output device or the Apple backend; those checks stay in the C consumer test
//! `tests/unit/c_api_consumer_test.cpp`.
mod common;
mod monitor;
mod options;
mod preview;
mod query;
mod render;
