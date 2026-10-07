//! Triple Balance numerical preparation and independent render state.
pub mod live;
pub mod panner;
pub mod processor;
pub mod session;
use crate::{Error, Result};
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Position {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}
impl Position {
    pub fn internal(self) -> Self {
        Self {
            x: (self.x + 1.) * 0.5,
            y: (1. - self.y) * 0.5,
            z: self.z,
        }
    }
    pub fn adm(self) -> Self {
        Self {
            x: self.x * 2. - 1.,
            y: 1. - self.y * 2.,
            z: self.z,
        }
    }
    pub fn advance(&mut self, target: Self) {
        let a = 1. - (-512f32 / 1200.).exp();
        self.x += a * (target.x - self.x);
        self.y += a * (target.y - self.y);
        self.z += a * (target.z - self.z);
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layout {
    Seven,
    Nine,
    Room222,
}
impl Layout {
    pub fn from_code(code: u32) -> Result<Self> {
        match code {
            0 => Ok(Self::Seven),
            1 => Ok(Self::Nine),
            2 => Ok(Self::Room222),
            _ => Err(invalid()),
        }
    }
    pub fn channels(self) -> usize {
        match self {
            Self::Seven => 12,
            Self::Nine => 16,
            Self::Room222 => 24,
        }
    }
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Event {
    pub start: u64,
    pub position: Position,
    pub size: f32,
}
pub(crate) fn invalid() -> Error {
    Error::InvalidArgument("Invalid Triple Balance parameters, buffers or state")
}
pub(crate) fn count(a: usize, b: usize) -> Result<usize> {
    a.checked_mul(b)
        .filter(|n| *n <= isize::MAX as usize / 8)
        .ok_or_else(invalid)
}
