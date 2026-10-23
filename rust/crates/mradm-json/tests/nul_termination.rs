//! End-of-input behavior of nlohmann/json 3.12.0, also used by the configuration parsers.
use mradm_json::{Value, parse};

#[test]
fn nul_ends_input_only_after_a_complete_root_value() {
    for text in [
        "{}", "[]", "true", "false", "null", "1", "-0.5e2", "\"text\"",
    ] {
        let expected = parse(text.as_bytes()).unwrap();
        for suffix in ["\0", " \r\n\0ignored", "\0{}"] {
            assert_eq!(
                parse(format!("{text}{suffix}").as_bytes()).unwrap(),
                expected
            );
        }
    }
    for text in ["\0{}", "[1\0]", "{\"a\":1\0}", "\"a\0b\"", "1e\0", "{}x\0"] {
        assert!(parse(text.as_bytes()).is_err(), "{text:?}");
    }
    assert_eq!(parse(br#""a\u0000b""#).unwrap(), Value::from("a\0b"));
}
