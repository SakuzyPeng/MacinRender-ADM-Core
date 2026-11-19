//! 渲染端的音频容器入口。BWF 实现由 macin-bwf 提供；CAF 归本仓库（FLAC 读取链随七期 7.2 A 删除）。
#![forbid(unsafe_code)]
pub use macin_bwf::*;
pub mod caf;
