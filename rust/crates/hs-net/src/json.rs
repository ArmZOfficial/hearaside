//! JSON text written byte for byte the way the C++ Hub wrote it: `juce::JSON::toString(var, /*allOnOneLine*/ true)`
//! (JUCE 8.0.15, `maximumDecimalPlaces` 15). Members and elements are separated by `", "`, a member name from its value
//! by `": "`, objects keep the order they were built in (`DynamicObject`), numbers go through JUCE's `serialiseDouble`
//! and `reduceLengthOfFloatString` (`"0.0"`, `"-6.0"`, `"0.100000001490116"` for the float 0.1, `"1.0e7"`), and a
//! number that is not finite is `null`. Strings are escaped like `ws::json_str` (R3.3).
//!
//! Used for the REST API's answers: `serde_json::Value` would sort the members and print numbers differently.
use crate::share::ws::json_str;

/// A JSON value as `juce::var` held it. An integer is an `Int` (printed as is), a `float` or `double` a `Num`.
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    /// members in insertion order
    Obj(Vec<(String, Json)>),
}

impl Json {
    /// An object from `(name, value)` pairs, in this order.
    pub fn obj<const N: usize>(fields: [(&str, Json); N]) -> Json {
        Json::Obj(fields.into_iter().map(|(k, v)| (k.to_owned(), v)).collect())
    }

    pub fn str(s: impl Into<String>) -> Json {
        Json::Str(s.into())
    }

    /// A `float` widened to `double` first, as `juce::var(float)` does.
    pub fn float(v: f32) -> Json {
        Json::Num(f64::from(v))
    }

    /// `{"error": message}`
    pub fn error(message: &str) -> Json {
        Json::obj([("error", Json::str(message))])
    }

    /// The member `name` of an object.
    pub fn get(&self, name: &str) -> Option<&Json> {
        match self {
            Json::Obj(m) => m.iter().find(|(k, _)| k == name).map(|(_, v)| v),
            _ => None,
        }
    }

    /// The text `JSON::toString(v, true)` gives.
    pub fn to_juce_string(&self) -> String {
        let mut out = String::new();
        self.write(&mut out);
        out
    }

    fn write(&self, out: &mut String) {
        match self {
            Json::Null => out.push_str("null"),
            Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
            Json::Int(i) => out.push_str(&i.to_string()),
            Json::Num(d) if d.is_finite() => out.push_str(&serialise_double(*d, 15)),
            Json::Num(_) => out.push_str("null"),
            Json::Str(s) => out.push_str(&json_str(s)),
            Json::Arr(a) => {
                out.push('[');
                for (i, v) in a.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    v.write(out);
                }
                out.push(']');
            }
            Json::Obj(m) => {
                out.push('{');
                for (i, (k, v)) in m.iter().enumerate() {
                    if i > 0 {
                        out.push_str(", ");
                    }
                    out.push_str(&json_str(k));
                    out.push_str(": ");
                    v.write(out);
                }
                out.push('}');
            }
        }
    }
}

/// `%.{places}e` as the C++ standard library prints it: a sign and at least two digits in the exponent.
fn scientific(v: f64, places: usize) -> String {
    let s = format!("{v:.places$e}");
    let (mantissa, exp) = s.split_once('e').unwrap_or((&s, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    format!(
        "{mantissa}e{}{:02}",
        if exp < 0 { '-' } else { '+' },
        exp.abs()
    )
}

/// `juce::reduceLengthOfFloatString` (juce_String.cpp), index for index: cuts the trailing zeros of the fraction (one
/// zero stays after the point), and the `+` and leading zeros of an exponent.
fn reduce_length_of_float_string(input: &str) -> String {
    let b = input.as_bytes();
    let end = b.len();
    let (mut trim_start, mut trim_end) = (end, end);
    let (mut exp_start, mut exp_end) = (end, end);
    let mut current = 0u8;
    let mut c = end;
    while c > 1 {
        c -= 1;
        current = b[c];
        if current == b'0' && c + 1 == trim_start {
            trim_start -= 1;
        } else if current == b'.' {
            if trim_start == c + 1 && trim_start != end && b[trim_start] == b'0' {
                trim_start += 1;
            }
            break;
        } else if current == b'e' || current == b'E' {
            let mut next = c + 1;
            if next != end {
                if b[next] == b'-' {
                    next += 1;
                }
                exp_start = next;
                if next != end && b[next] == b'+' {
                    next += 1;
                }
                exp_end = next;
            }
            while next != end {
                let ch = b[next];
                next += 1;
                if ch != b'0' {
                    break;
                }
                exp_end = next;
            }
            if exp_end == end {
                exp_start = c;
            }
            trim_start = c;
            trim_end = trim_start;
        }
    }
    let s = input;
    if (trim_start != trim_end && current == b'.') || exp_start != exp_end {
        if trim_start == trim_end {
            return format!("{}{}", &s[..exp_start], &s[exp_end..]);
        }
        if exp_start == exp_end {
            return format!("{}{}", &s[..trim_start], &s[trim_end..]);
        }
        if trim_end == exp_start {
            return format!("{}{}", &s[..trim_start], &s[exp_end..]);
        }
        return format!(
            "{}{}{}",
            &s[..trim_start],
            &s[trim_end..exp_start],
            &s[exp_end..]
        );
    }
    input.to_owned()
}

/// `juce::serialiseDouble(input, maxDecimalPlaces)` for a finite `input`; `<= 0` = "as many places as needed".
pub(crate) fn serialise_double(input: f64, max_places: i32) -> String {
    let abs = input.abs();
    if abs >= 1.0e6 || abs <= 1.0e-5 {
        let places = if max_places > 0 { max_places } else { 15 };
        return reduce_length_of_float_string(&scientific(input, places as usize));
    }
    if f64::from(input as i32) == input {
        return format!("{input:.1}");
    }
    let places = if max_places > 0 {
        max_places
    } else if abs < 1.0 {
        if abs >= 1.0e-3 {
            if abs >= 1.0e-1 {
                16
            } else if abs >= 1.0e-2 {
                17
            } else {
                18
            }
        } else if abs >= 1.0e-4 {
            19
        } else {
            20
        }
    } else if abs < 1.0e3 {
        if abs < 1.0e1 {
            15
        } else if abs < 1.0e2 {
            14
        } else {
            13
        }
    } else if abs < 1.0e4 {
        12
    } else if abs < 1.0e5 {
        11
    } else {
        10
    };
    reduce_length_of_float_string(&format!("{input:.*}", places as usize))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serialise_double_matches_the_juce_unit_test_table() {
        // juce_String.cpp, "Serialisation" (serialiseDouble with no limit on the places)
        let table: &[(f64, &str)] = &[
            (364.0, "364.0"),
            (1e7, "1.0e7"),
            (12345678901.0, "1.2345678901e10"),
            (1234567890123456.7, "1.234567890123457e15"),
            (12345678.901234567, "1.234567890123457e7"),
            (1234567.8901234567, "1.234567890123457e6"),
            (123456.78901234567, "123456.7890123457"),
            (12345.678901234567, "12345.67890123457"),
            (1234.5678901234567, "1234.567890123457"),
            (123.45678901234567, "123.4567890123457"),
            (12.345678901234567, "12.34567890123457"),
            (1.2345678901234567, "1.234567890123457"),
            (0.12345678901234567, "0.1234567890123457"),
            (0.012345678901234567, "0.01234567890123457"),
            (0.0012345678901234567, "0.001234567890123457"),
            (0.00012345678901234567, "0.0001234567890123457"),
            (0.000012345678901234567, "0.00001234567890123457"),
            (0.0000012345678901234567, "1.234567890123457e-6"),
            (0.00000012345678901234567, "1.234567890123457e-7"),
        ];
        for &(v, want) in table {
            assert_eq!(serialise_double(v, 0), want, "{v}");
            assert_eq!(serialise_double(-v, 0), format!("-{want}"), "-{v}");
        }
        for places in [0, 1, 2, 3, 10] {
            assert_eq!(serialise_double(1.0, places), "1.0");
        }
        let places = |p| serialise_double(4.567, p);
        assert_eq!(
            [places(0), places(1), places(2), places(3), places(10)],
            ["4.567", "4.6", "4.57", "4.567", "4.567"]
        );
    }

    #[test]
    fn numbers_as_the_rest_api_prints_them() {
        let n = |v: f64| Json::Num(v).to_juce_string();
        assert_eq!(n(0.0), "0.0");
        assert_eq!(n(-0.0), "-0.0");
        assert_eq!(n(-6.0), "-6.0");
        assert_eq!(n(120.0), "120.0");
        assert_eq!(n(500.0), "500.0");
        assert_eq!(n(0.5), "0.5");
        // a float widened to a double shows its binary value (15 places)
        assert_eq!(Json::float(0.1).to_juce_string(), "0.100000001490116");
        assert_eq!(Json::float(-0.1).to_juce_string(), "-0.100000001490116");
        assert_eq!(Json::float(23.4).to_juce_string(), "23.399999618530273");
        assert_eq!(n(-16.2), "-16.199999999999999");
        assert_eq!(n(1.0e6), "1.0e6");
        assert_eq!(n(0.000001), "1.0e-6");
        assert_eq!(n(f64::NAN), "null");
        assert_eq!(n(f64::INFINITY), "null");
    }

    /// Output of `juce::JSON::toString(var(x), true)` from a juce_core 8.0.15 build (MSVC 14.44), one line per input.
    #[test]
    fn numbers_equal_what_the_juce_build_printed() {
        let floats: &[(f32, &str)] = &[
            (0.1, "0.100000001490116"),
            (-0.1, "-0.100000001490116"),
            (0.0, "0.0"),
            (-0.0, "-0.0"),
            (120.0, "120.0"),
            (-6.0, "-6.0"),
            (12.0, "12.0"),
            (0.5, "0.5"),
            (23.4, "23.399999618530273"),
            (1234.5678, "1234.5677490234375"),
            (-59.9, "-59.900001525878906"),
            (0.3, "0.300000011920929"),
            (7.7, "7.699999809265137"),
            (499.5, "499.5"),
            (0.0001, "0.000099999997474"),
            (0.00001, "9.999999747378752e-6"),
            (100000.5, "100000.5"),
            (1.0e6, "1.0e6"),
            (12345.678, "12345.677734375"),
            (0.05, "0.050000000745058"),
            (-16.2, "-16.200000762939453"),
            (1e-7, "1.000000011686097e-7"),
        ];
        for &(v, want) in floats {
            assert_eq!(Json::float(v).to_juce_string(), want, "float {v}");
        }
        let doubles: &[(f64, &str)] = &[
            (23.4, "23.399999999999999"),
            (-16.2, "-16.199999999999999"),
            (-16.0, "-16.0"),
            (0.0, "0.0"),
            (-0.0, "-0.0"),
            (0.1, "0.1"),
            (255.6, "255.599999999999994"),
            (12.3, "12.300000000000001"),
            (-70.0, "-70.0"),
            (-69.9, "-69.900000000000006"),
            (1e-5, "1.0e-5"),
            (1.0e6, "1.0e6"),
            (999999.5, "999999.5"),
            (0.000001, "1.0e-6"),
            (1234.5678, "1234.567800000000034"),
        ];
        for &(v, want) in doubles {
            assert_eq!(Json::Num(v).to_juce_string(), want, "double {v}");
        }
    }

    #[test]
    fn objects_arrays_and_strings_use_the_one_line_layout() {
        let j = Json::obj([
            ("b", Json::Int(-3)),
            ("a", Json::Bool(true)),
            ("n", Json::Null),
            ("s", Json::str("x\"y\u{1}ก")),
            (
                "l",
                Json::Arr(vec![Json::Int(1), Json::Arr(vec![]), Json::obj([])]),
            ),
        ]);
        assert_eq!(
            j.to_juce_string(),
            "{\"b\": -3, \"a\": true, \"n\": null, \"s\": \"x\\\"y\\u0001ก\", \"l\": [1, [], {}]}"
        );
        assert_eq!(j.get("a"), Some(&Json::Bool(true)));
        assert_eq!(j.get("zz"), None);
    }
}
