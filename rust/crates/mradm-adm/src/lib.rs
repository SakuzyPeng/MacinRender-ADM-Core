//! Project-owned ADM metadata. Container I/O and the public scene remain in C++.
#![forbid(unsafe_code)]

mod document;
mod generate;
mod patch;
mod project;
pub mod records;
mod xml;

pub use document::Document;
pub use generate::{GeneratedMetadata, Speaker, generate};
pub use patch::Patch;
pub use project::Snapshot;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub message: String,
}

impl Error {
    pub fn invalid(message: impl Into<String>) -> Self {
        Self {
            code: 1,
            message: message.into(),
        }
    }
    pub fn unsupported(message: impl Into<String>) -> Self {
        Self {
            code: 2,
            message: message.into(),
        }
    }
    pub fn xml(message: impl Into<String>) -> Self {
        Self {
            code: 3,
            message: message.into(),
        }
    }
}
pub type Result<T> = std::result::Result<T, Error>;
