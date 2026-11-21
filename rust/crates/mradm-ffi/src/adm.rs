//! The borrowed text record of the private C boundary (`MradmAdmText`).

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Text {
    pub data: *const u8,
    pub len: usize,
}
impl Text {
    pub fn new(s: &str) -> Self {
        Self::bytes(s.as_bytes())
    }
    pub fn bytes(s: &[u8]) -> Self {
        Self {
            data: s.as_ptr(),
            len: s.len(),
        }
    }
}
