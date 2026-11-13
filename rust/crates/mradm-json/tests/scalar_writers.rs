use std::fmt;

struct Limited {
    bytes: [u8; 128],
    len: usize,
    capacity: usize,
}

impl fmt::Write for Limited {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        if text.len() > self.capacity - self.len {
            return Err(fmt::Error);
        }
        self.bytes[self.len..self.len + text.len()].copy_from_slice(text.as_bytes());
        self.len += text.len();
        Ok(())
    }
}

fn check(expected: &str, write: impl Fn(&mut Limited) -> fmt::Result) {
    for capacity in 0..=expected.len() {
        let mut out = Limited {
            bytes: [0; 128],
            len: 0,
            capacity,
        };
        assert_eq!(write(&mut out).is_ok(), capacity == expected.len());
        if capacity == expected.len() {
            assert_eq!(&out.bytes[..out.len], expected.as_bytes());
        }
    }
}

#[test]
fn doubles_propagate_sink_errors() {
    for (value, expected) in [
        (-0.0, "-0.0"),
        (1.25, "1.25"),
        (100.0, "100.0"),
        (0.0001, "0.0001"),
        (1e-9, "1e-09"),
        (1e20, "1e+20"),
        (-1e100, "-1e+100"),
        (f64::MAX, "1.7976931348623157e+308"),
        (f64::INFINITY, "null"),
        (f64::NAN, "null"),
    ] {
        check(expected, |out| mradm_json::write_f64(out, value));
    }
}

#[test]
fn strings_propagate_sink_errors() {
    check("\"\"", |out| mradm_json::write_string(out, ""));
    check(
        "\"\\\"\\\\\\u0000\\u0001\\b\\t\\n\\f\\r准备中/\u{2028}\"",
        |out| mradm_json::write_string(out, "\"\\\0\u{1}\u{8}\t\n\u{c}\r准备中/\u{2028}"),
    );
}
