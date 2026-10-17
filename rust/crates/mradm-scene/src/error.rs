/// Which locator the C++ boundary attaches to an import error. The caller composes it with its
/// native path, so the reported path is the caller's spelling rather than the UTF-8 conversion.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Context {
    None,
    /// `input=<path>`, the importer's own locator.
    Input,
    /// `path=<path>`, the locator of the WAVE reader errors that pass through unchanged.
    Path,
    /// The caller's source label as given: a policy file path or `<memory>`.
    Label,
}

/// Codes follow `adm::ErrorCode`; messages are the user-visible text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub message: String,
    pub context: Context,
}

impl Error {
    pub fn new(code: i32, message: impl Into<String>, context: Context) -> Self {
        Self {
            code,
            message: message.into(),
            context,
        }
    }
    pub fn invalid(message: impl Into<String>, context: Context) -> Self {
        Self::new(1, message, context)
    }
    pub fn unsupported(message: impl Into<String>, context: Context) -> Self {
        Self::new(2, message, context)
    }
    pub fn io(message: impl Into<String>, context: Context) -> Self {
        Self::new(3, message, context)
    }
    pub(crate) fn wav(error: mradm_wav::Error, context: Context) -> Self {
        Self::new(error.code, error.message, context)
    }
    pub(crate) fn adm(error: mradm_adm::Error) -> Self {
        Self::new(error.code, error.message, Context::Input)
    }
}

pub type Result<T> = std::result::Result<T, Error>;
