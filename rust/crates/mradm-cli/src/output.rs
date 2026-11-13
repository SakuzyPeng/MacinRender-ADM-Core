//! What the CLI writes: text on stdout, parse errors on stderr, and the log lines spdlog wrote
//! before (`[level] message` on stdout, the level coloured on a colour terminal).
use std::io::{IsTerminal, Write};

/// Writes `bytes` to stdout and flushes, as each `fmt::print` / spdlog line reached the stream.
pub(crate) fn stdout(bytes: &[u8]) {
    let mut out = std::io::stdout().lock();
    let _ = out.write_all(bytes);
    let _ = out.flush();
}

pub(crate) fn stderr(bytes: &[u8]) {
    let mut err = std::io::stderr().lock();
    let _ = err.write_all(bytes);
    let _ = err.flush();
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Level {
    Debug,
    Info,
    Warning,
    Error,
}

impl Level {
    fn name(self) -> &'static str {
        match self {
            Level::Debug => "debug",
            Level::Info => "info",
            Level::Warning => "warning",
            Level::Error => "error",
        }
    }

    /// spdlog's ANSI colour of the level.
    fn colour(self) -> &'static str {
        match self {
            Level::Debug => "\x1b[36m",
            Level::Info => "\x1b[32m",
            Level::Warning => "\x1b[33m\x1b[1m",
            Level::Error => "\x1b[31m\x1b[1m",
        }
    }

    /// An `adm::LogLevel` value.
    pub(crate) fn of(level: u32) -> Option<Self> {
        match level {
            0 => Some(Level::Debug),
            1 => Some(Level::Info),
            2 => Some(Level::Warning),
            3 => Some(Level::Error),
            _ => None,
        }
    }
}

/// spdlog's `is_color_terminal`: `COLORTERM` set, or a `TERM` naming a colour terminal.
fn colour_terminal() -> bool {
    if std::env::var_os("COLORTERM").is_some() {
        return true;
    }
    const TERMS: [&str; 16] = [
        "ansi",
        "color",
        "console",
        "cygwin",
        "gnome",
        "konsole",
        "kterm",
        "linux",
        "msys",
        "putty",
        "rxvt",
        "screen",
        "vt100",
        "xterm",
        "alacritty",
        "vt102",
    ];
    std::env::var("TERM").is_ok_and(|term| TERMS.iter().any(|t| term.contains(t)))
}

/// The default logger: info, or debug with `-v`.
pub(crate) struct Logger {
    minimum: Level,
    colour: bool,
}

impl Logger {
    pub(crate) fn new(verbose: bool) -> Self {
        Self {
            minimum: if verbose { Level::Debug } else { Level::Info },
            // The Windows console colours of spdlog's wincolor sink are not reproduced.
            colour: !cfg!(windows) && std::io::stdout().is_terminal() && colour_terminal(),
        }
    }

    /// `spdlog` with the pattern `%^[%l]%$ %v`.
    pub(crate) fn log(&self, level: Level, message: &[u8]) {
        if level < self.minimum {
            return;
        }
        let mut line = Vec::with_capacity(message.len() + 16);
        if self.colour {
            line.extend_from_slice(level.colour().as_bytes());
        }
        line.push(b'[');
        line.extend_from_slice(level.name().as_bytes());
        line.push(b']');
        if self.colour {
            line.extend_from_slice(b"\x1b[m");
        }
        line.push(b' ');
        line.extend_from_slice(message);
        line.push(b'\n');
        stdout(&line);
    }

    pub(crate) fn debug(&self, message: &[u8]) {
        self.log(Level::Debug, message);
    }

    pub(crate) fn info(&self, message: &[u8]) {
        self.log(Level::Info, message);
    }

    pub(crate) fn warn(&self, message: &[u8]) {
        self.log(Level::Warning, message);
    }

    pub(crate) fn error(&self, message: &[u8]) {
        self.log(Level::Error, message);
    }
}

/// fmt's display width of `text`: two columns for the East Asian wide ranges fmt counts.
fn width(text: &str) -> usize {
    text.chars()
        .map(|c| {
            let cp = c as u32;
            let wide = (0x1100..=0x115f).contains(&cp)
                || cp == 0x2329
                || cp == 0x232a
                || ((0x2e80..=0xa4cf).contains(&cp) && cp != 0x303f)
                || (0xac00..=0xd7a3).contains(&cp)
                || (0xf900..=0xfaff).contains(&cp)
                || (0xfe10..=0xfe19).contains(&cp)
                || (0xfe30..=0xfe6f).contains(&cp)
                || (0xff00..=0xff60).contains(&cp)
                || (0xffe0..=0xffe6).contains(&cp)
                || (0x20000..=0x2fffd).contains(&cp)
                || (0x30000..=0x3fffd).contains(&cp)
                || (0x1f300..=0x1f64f).contains(&cp)
                || (0x1f900..=0x1f9ff).contains(&cp);
            if wide { 2 } else { 1 }
        })
        .sum()
}

/// `{:<n}` of fmt: `text` padded on the right to `n` columns with `fill`.
pub(crate) fn left(text: &str, n: usize, fill: char) -> String {
    let mut out = String::from(text);
    for _ in width(text)..n {
        out.push(fill);
    }
    out
}

/// `{:>n}` of fmt.
pub(crate) fn right(text: &str, n: usize) -> String {
    let mut out = String::new();
    for _ in width(text)..n {
        out.push(' ');
    }
    out.push_str(text);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn padding_counts_wide_characters_twice() {
        assert_eq!(left("ab", 4, ' '), "ab  ");
        assert_eq!(left("精确", 6, ' '), "精确  ");
        assert_eq!(left("", 3, '-'), "---");
        assert_eq!(right("7", 3), "  7");
        assert_eq!(left("longer", 2, ' '), "longer");
    }
}
