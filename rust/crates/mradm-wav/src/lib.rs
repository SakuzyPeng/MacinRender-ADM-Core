//! 渲染端的音频容器入口。BWF 实现由 macin-bwf 提供；CAF/FLAC 继续归本仓库。
#![forbid(unsafe_code)]
pub use macin_bwf::*;
pub mod caf;
pub mod flac;
