//! A tiny JSON parser (no dependencies) used to load mod files.
//!
//! Behaviour that matters for mods, and that the C++ port (`cpp/json.cpp`) reproduces exactly:
//! * object keys keep their file order and duplicate keys are an error,
//! * errors carry `line L, column C` (1-based, counted in bytes),
//! * nesting is limited to `MAX_DEPTH`, numbers must be finite.

pub const MAX_DEPTH: usize = 32;

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Num(f64),
    /// Only produced by exporters: prints as the shortest decimal that round-trips an `f32`.
    Float(f32),
    Str(String),
    Arr(Vec<Value>),
    Obj(Vec<(String, Value)>),
}

impl Value {
    pub fn type_name(&self) -> &'static str {
        match self {
            Value::Null => "null",
            Value::Bool(_) => "a boolean",
            Value::Num(_) | Value::Float(_) => "a number",
            Value::Str(_) => "a string",
            Value::Arr(_) => "an array",
            Value::Obj(_) => "an object",
        }
    }
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

pub fn parse(text: &str) -> Result<Value, String> {
    let mut p = Parser { s: text.as_bytes(), i: 0 };
    p.ws();
    let v = p.value(0)?;
    p.ws();
    if p.i < p.s.len() {
        return Err(p.fail("unexpected data after the end of the JSON value"));
    }
    Ok(v)
}

impl<'a> Parser<'a> {
    fn fail(&self, msg: &str) -> String {
        let mut line = 1;
        let mut col = 1;
        for &b in &self.s[..self.i.min(self.s.len())] {
            if b == b'\n' {
                line += 1;
                col = 1;
            } else {
                col += 1;
            }
        }
        format!("line {}, column {}: {}", line, col, msg)
    }

    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn value(&mut self, depth: usize) -> Result<Value, String> {
        if depth > MAX_DEPTH {
            return Err(self.fail("nesting is too deep"));
        }
        match self.peek() {
            None => Err(self.fail("unexpected end of file")),
            Some(b'{') => self.object(depth),
            Some(b'[') => self.array(depth),
            Some(b'"') => Ok(Value::Str(self.string()?)),
            Some(b't') => self.literal("true", Value::Bool(true)),
            Some(b'f') => self.literal("false", Value::Bool(false)),
            Some(b'n') => self.literal("null", Value::Null),
            Some(b'-') | Some(b'0'..=b'9') => self.number(),
            Some(_) => Err(self.fail("unexpected character")),
        }
    }

    fn literal(&mut self, word: &str, v: Value) -> Result<Value, String> {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            Ok(v)
        } else {
            Err(self.fail("unexpected character"))
        }
    }

    fn number(&mut self) -> Result<Value, String> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek() {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.i += 1;
                }
            }
            _ => return Err(self.fail("invalid number")),
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.fail("invalid number"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        if matches!(self.peek(), Some(b'e') | Some(b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+') | Some(b'-')) {
                self.i += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.fail("invalid number"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        let text = std::str::from_utf8(&self.s[start..self.i]).unwrap_or("0");
        match text.parse::<f64>() {
            Ok(v) if v.is_finite() => Ok(Value::Num(v)),
            _ => Err(self.fail("number is out of range")),
        }
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let mut v = 0u32;
        for _ in 0..4 {
            let d = match self.peek() {
                Some(c @ b'0'..=b'9') => (c - b'0') as u32,
                Some(c @ b'a'..=b'f') => (c - b'a') as u32 + 10,
                Some(c @ b'A'..=b'F') => (c - b'A') as u32 + 10,
                _ => return Err(self.fail("invalid \\u escape")),
            };
            v = v * 16 + d;
            self.i += 1;
        }
        Ok(v)
    }

    fn string(&mut self) -> Result<String, String> {
        self.i += 1; // opening quote
        let mut out: Vec<u8> = Vec::new();
        loop {
            let c = match self.peek() {
                None => return Err(self.fail("unterminated string")),
                Some(c) => c,
            };
            match c {
                b'"' => {
                    self.i += 1;
                    // the input was valid UTF-8 and escapes only produce valid scalars
                    return Ok(String::from_utf8(out).unwrap_or_default());
                }
                b'\\' => {
                    self.i += 1;
                    let e = match self.peek() {
                        None => return Err(self.fail("unterminated string")),
                        Some(e) => e,
                    };
                    self.i += 1;
                    match e {
                        b'"' => out.push(b'"'),
                        b'\\' => out.push(b'\\'),
                        b'/' => out.push(b'/'),
                        b'b' => out.push(8),
                        b'f' => out.push(12),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xD800..0xDC00).contains(&cp) {
                                if self.peek() == Some(b'\\') && self.s.get(self.i + 1) == Some(&b'u') {
                                    self.i += 2;
                                    let lo = self.hex4()?;
                                    if !(0xDC00..0xE000).contains(&lo) {
                                        return Err(self.fail("invalid surrogate pair"));
                                    }
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    return Err(self.fail("invalid surrogate pair"));
                                }
                            } else if (0xDC00..0xE000).contains(&cp) {
                                return Err(self.fail("invalid surrogate pair"));
                            }
                            let ch = char::from_u32(cp).unwrap_or('\u{FFFD}');
                            let mut buf = [0u8; 4];
                            out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                        }
                        _ => {
                            self.i -= 1;
                            return Err(self.fail("invalid escape sequence"));
                        }
                    }
                }
                0..=0x1f => return Err(self.fail("control character in string")),
                _ => {
                    out.push(c);
                    self.i += 1;
                }
            }
        }
    }

    fn array(&mut self, depth: usize) -> Result<Value, String> {
        self.i += 1;
        let mut items = Vec::new();
        self.ws();
        if self.peek() == Some(b']') {
            self.i += 1;
            return Ok(Value::Arr(items));
        }
        loop {
            self.ws();
            items.push(self.value(depth + 1)?);
            self.ws();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b']') => {
                    self.i += 1;
                    return Ok(Value::Arr(items));
                }
                None => return Err(self.fail("unexpected end of file")),
                Some(_) => return Err(self.fail("expected ',' or ']'")),
            }
        }
    }

    fn object(&mut self, depth: usize) -> Result<Value, String> {
        self.i += 1;
        let mut items: Vec<(String, Value)> = Vec::new();
        self.ws();
        if self.peek() == Some(b'}') {
            self.i += 1;
            return Ok(Value::Obj(items));
        }
        loop {
            self.ws();
            if self.peek() != Some(b'"') {
                return Err(self.fail(if self.peek().is_none() { "unexpected end of file" } else { "expected a string key" }));
            }
            let key_at = self.i;
            let key = self.string()?;
            if items.iter().any(|(k, _)| *k == key) {
                self.i = key_at;
                return Err(self.fail(&format!("duplicate key \"{}\"", key)));
            }
            self.ws();
            if self.peek() != Some(b':') {
                return Err(self.fail(if self.peek().is_none() { "unexpected end of file" } else { "expected ':'" }));
            }
            self.i += 1;
            self.ws();
            let v = self.value(depth + 1)?;
            items.push((key, v));
            self.ws();
            match self.peek() {
                Some(b',') => self.i += 1,
                Some(b'}') => {
                    self.i += 1;
                    return Ok(Value::Obj(items));
                }
                None => return Err(self.fail("unexpected end of file")),
                Some(_) => return Err(self.fail("expected ',' or '}'")),
            }
        }
    }
}

// ------------------------------------------------------------------------------ writing

/// JSON string literal. Only `"`, `\\`, `\n` and control characters are escaped.
pub fn quote(v: &str) -> String {
    let mut s = String::with_capacity(v.len() + 2);
    s.push('"');
    for c in v.chars() {
        match c {
            '"' => s.push_str("\\\""),
            '\\' => s.push_str("\\\\"),
            '\n' => s.push_str("\\n"),
            c if (c as u32) < 0x20 => s.push_str(&format!("\\u{:04x}", c as u32)),
            c => s.push(c),
        }
    }
    s.push('"');
    s
}

/// Shortest decimal for an `f32` (never exponent form); non-finite values print as `0`.
pub fn fmt_f32(v: f32) -> String {
    if v.is_finite() {
        format!("{}", v)
    } else {
        "0".to_string()
    }
}

fn scalar(v: &Value) -> bool {
    !matches!(v, Value::Arr(_) | Value::Obj(_))
}

fn write_value(v: &Value, indent: usize, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Num(n) => out.push_str(&format!("{}", n)),
        Value::Float(f) => out.push_str(&fmt_f32(*f)),
        Value::Str(t) => out.push_str(&quote(t)),
        Value::Arr(a) if a.is_empty() => out.push_str("[]"),
        Value::Arr(a) if a.iter().all(scalar) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push_str(", ");
                }
                write_value(x, indent, out);
            }
            out.push(']');
        }
        Value::Arr(a) => {
            out.push_str("[\n");
            for (i, x) in a.iter().enumerate() {
                out.push_str(&" ".repeat(indent + 2));
                write_value(x, indent + 2, out);
                out.push_str(if i + 1 < a.len() { ",\n" } else { "\n" });
            }
            out.push_str(&" ".repeat(indent));
            out.push(']');
        }
        Value::Obj(o) if o.is_empty() => out.push_str("{}"),
        Value::Obj(o) => {
            out.push_str("{\n");
            for (i, (k, x)) in o.iter().enumerate() {
                out.push_str(&" ".repeat(indent + 2));
                out.push_str(&quote(k));
                out.push_str(": ");
                write_value(x, indent + 2, out);
                out.push_str(if i + 1 < o.len() { ",\n" } else { "\n" });
            }
            out.push_str(&" ".repeat(indent));
            out.push('}');
        }
    }
}

/// Human-readable JSON: objects one key per line, arrays of scalars on one line.
pub fn to_pretty(v: &Value) -> String {
    let mut s = String::new();
    write_value(v, 0, &mut s);
    s.push('\n');
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_values_and_keeps_key_order() {
        let v = parse(r#" {"b": [1, 2.5, -3e2, true, null], "a": "xé\n"} "#).unwrap();
        match v {
            Value::Obj(o) => {
                assert_eq!(o[0].0, "b");
                assert_eq!(o[1].0, "a");
                assert_eq!(o[1].1, Value::Str("xé\n".into()));
                assert_eq!(o[0].1, Value::Arr(vec![Value::Num(1.0), Value::Num(2.5), Value::Num(-300.0), Value::Bool(true), Value::Null]));
            }
            _ => panic!(),
        }
        assert_eq!(parse(r#""😀""#).unwrap(), Value::Str("😀".into()));
    }

    #[test]
    fn reports_position_and_rejects_bad_input() {
        assert_eq!(parse("{\n  \"a\": }").unwrap_err(), "line 2, column 8: unexpected character");
        assert!(parse("[1,]").is_err());
        assert!(parse("{\"a\":1,\"a\":2}").unwrap_err().contains("duplicate key \"a\""));
        assert!(parse("01").is_err());
        assert!(parse("1e999").unwrap_err().contains("out of range"));
        assert!(parse("[1] x").unwrap_err().contains("after the end"));
        assert!(parse("\"a\nb\"").unwrap_err().contains("control character"));
        assert!(parse(&"[".repeat(40)).unwrap_err().contains("too deep"));
    }

    #[test]
    fn pretty_printer_round_trips() {
        let v = Value::Obj(vec![
            ("a".into(), Value::Arr(vec![Value::Float(0.1), Value::Num(2.0)])),
            ("b".into(), Value::Arr(vec![Value::Obj(vec![("x".into(), Value::Str("q\"\n".into()))])])),
            ("c".into(), Value::Obj(vec![])),
        ]);
        let t = to_pretty(&v);
        assert!(t.contains("[0.1, 2]"));
        let back = parse(&t).unwrap();
        // Float(0.1) re-parses as the f64 nearest to 0.1, which casts back to the same f32
        match &back {
            Value::Obj(o) => match &o[0].1 {
                Value::Arr(a) => assert_eq!(a[0], Value::Num(0.1)),
                _ => panic!(),
            },
            _ => panic!(),
        }
        assert_eq!(quote("a\tb"), "\"a\\u0009b\"");
    }
}
