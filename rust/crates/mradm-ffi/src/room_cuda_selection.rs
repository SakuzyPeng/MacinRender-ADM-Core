use mradm_dsp::{Error, Result, room::quality::Quality};

fn resolve(
    quality: Quality,
    inherited: Option<bool>,
    requested: Option<&str>,
    available: bool,
) -> Result<bool> {
    if quality == Quality::Exact {
        return Ok(false);
    }
    let enabled = if let Some(value) = inherited {
        value
    } else {
        match requested {
            None | Some("0") => false,
            Some("1") => true,
            _ => return Err(Error::InvalidArgument("MR_ADM_ROOM_CUDA must be 0 or 1")),
        }
    };
    if enabled && !available {
        return Err(Error::Unsupported(
            "Optional Windows ROOM CUDA support is not enabled in this build",
        ));
    }
    Ok(enabled)
}
pub fn selected(quality: Quality, inherited: Option<bool>) -> Result<bool> {
    // Exact never observes the opt-in. A room switch keeps its session's backend.
    if quality == Quality::Exact {
        return Ok(false);
    }
    let value = if inherited.is_none() {
        std::env::var("MR_ADM_ROOM_CUDA")
            .map(Some)
            .or_else(|error| match error {
                std::env::VarError::NotPresent => Ok(None),
                _ => Err(Error::InvalidArgument("Invalid ROOM CUDA selection")),
            })?
    } else {
        None
    };
    resolve(
        quality,
        inherited,
        value.as_deref(),
        cfg!(all(windows, feature = "room-cuda")),
    )
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_default_and_inherited_backends_are_stable() {
        assert!(!resolve(Quality::Exact, Some(true), Some("invalid"), false).unwrap());
        assert!(!resolve(Quality::Realtime, None, None, true).unwrap());
        assert!(!resolve(Quality::Realtime, Some(false), Some("1"), true).unwrap());
        assert!(resolve(Quality::Realtime, Some(true), Some("0"), true).unwrap());
        assert!(resolve(Quality::Realtime, None, Some("1"), false).is_err());
        assert!(resolve(Quality::Realtime, None, Some("invalid"), true).is_err());
    }
}
