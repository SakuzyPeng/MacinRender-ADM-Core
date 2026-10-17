//! Shortest-ish double formatting translated from nlohmann/json 3.12.0 `detail::dtoa_impl` and
//! `detail::to_chars` (Grisu2 after Florian Loitsch, "Printing Floating-Point Numbers Quickly and
//! Accurately with Integers", PLDI 2010). The digit generation and the fixed/exponent layout must
//! match nlohmann byte for byte; Grisu2 is not always shortest and may pick a different last digit
//! than Ryu, so serde_json's output is not interchangeable.

#[derive(Clone, Copy)]
struct DiyFp {
    f: u64,
    e: i32,
}

impl DiyFp {
    fn sub(x: Self, y: Self) -> Self {
        debug_assert!(x.e == y.e && x.f >= y.f);
        Self {
            f: x.f - y.f,
            e: x.e,
        }
    }

    /// The upper 64 bits of the 128-bit product, rounded (ties up).
    fn mul(x: Self, y: Self) -> Self {
        let u_lo = x.f & 0xFFFF_FFFF;
        let u_hi = x.f >> 32;
        let v_lo = y.f & 0xFFFF_FFFF;
        let v_hi = y.f >> 32;
        let p0 = u_lo * v_lo;
        let p1 = u_lo * v_hi;
        let p2 = u_hi * v_lo;
        let p3 = u_hi * v_hi;
        let mut q = (p0 >> 32) + (p1 & 0xFFFF_FFFF) + (p2 & 0xFFFF_FFFF);
        q += 1 << 31;
        Self {
            f: p3 + (p2 >> 32) + (p1 >> 32) + (q >> 32),
            e: x.e + y.e + 64,
        }
    }

    fn normalize(mut x: Self) -> Self {
        debug_assert!(x.f != 0);
        while x.f >> 63 == 0 {
            x.f <<= 1;
            x.e -= 1;
        }
        x
    }

    fn normalize_to(x: Self, target_exponent: i32) -> Self {
        let delta = x.e - target_exponent;
        debug_assert!(delta >= 0 && ((x.f << delta) >> delta) == x.f);
        Self {
            f: x.f << delta,
            e: target_exponent,
        }
    }
}

struct Boundaries {
    w: DiyFp,
    minus: DiyFp,
    plus: DiyFp,
}

/// `value` must be finite and positive.
fn compute_boundaries(value: f64) -> Boundaries {
    const PRECISION: i32 = 53;
    const BIAS: i32 = 1024 - 1 + (PRECISION - 1);
    const MIN_EXP: i32 = 1 - BIAS;
    const HIDDEN_BIT: u64 = 1 << (PRECISION - 1);
    let bits = value.to_bits();
    let e = bits >> (PRECISION - 1);
    let f = bits & (HIDDEN_BIT - 1);
    let v = if e == 0 {
        DiyFp { f, e: MIN_EXP }
    } else {
        DiyFp {
            f: f + HIDDEN_BIT,
            e: e as i32 - BIAS,
        }
    };
    let lower_boundary_is_closer = f == 0 && e > 1;
    let m_plus = DiyFp {
        f: 2 * v.f + 1,
        e: v.e - 1,
    };
    let m_minus = if lower_boundary_is_closer {
        DiyFp {
            f: 4 * v.f - 1,
            e: v.e - 2,
        }
    } else {
        DiyFp {
            f: 2 * v.f - 1,
            e: v.e - 1,
        }
    };
    let w_plus = DiyFp::normalize(m_plus);
    let w_minus = DiyFp::normalize_to(m_minus, w_plus.e);
    Boundaries {
        w: DiyFp::normalize(v),
        minus: w_minus,
        plus: w_plus,
    }
}

const ALPHA: i32 = -60;

struct CachedPower {
    f: u64,
    e: i32,
    k: i32,
}

#[rustfmt::skip]
const CACHED_POWERS: [(u64, i32, i32); 79] = [
    (0xAB70FE17C79AC6CA, -1060, -300), (0xFF77B1FCBEBCDC4F, -1034, -292), (0xBE5691EF416BD60C, -1007, -284),
    (0x8DD01FAD907FFC3C, -980, -276), (0xD3515C2831559A83, -954, -268), (0x9D71AC8FADA6C9B5, -927, -260),
    (0xEA9C227723EE8BCB, -901, -252), (0xAECC49914078536D, -874, -244), (0x823C12795DB6CE57, -847, -236),
    (0xC21094364DFB5637, -821, -228), (0x9096EA6F3848984F, -794, -220), (0xD77485CB25823AC7, -768, -212),
    (0xA086CFCD97BF97F4, -741, -204), (0xEF340A98172AACE5, -715, -196), (0xB23867FB2A35B28E, -688, -188),
    (0x84C8D4DFD2C63F3B, -661, -180), (0xC5DD44271AD3CDBA, -635, -172), (0x936B9FCEBB25C996, -608, -164),
    (0xDBAC6C247D62A584, -582, -156), (0xA3AB66580D5FDAF6, -555, -148), (0xF3E2F893DEC3F126, -529, -140),
    (0xB5B5ADA8AAFF80B8, -502, -132), (0x87625F056C7C4A8B, -475, -124), (0xC9BCFF6034C13053, -449, -116),
    (0x964E858C91BA2655, -422, -108), (0xDFF9772470297EBD, -396, -100), (0xA6DFBD9FB8E5B88F, -369, -92),
    (0xF8A95FCF88747D94, -343, -84), (0xB94470938FA89BCF, -316, -76), (0x8A08F0F8BF0F156B, -289, -68),
    (0xCDB02555653131B6, -263, -60), (0x993FE2C6D07B7FAC, -236, -52), (0xE45C10C42A2B3B06, -210, -44),
    (0xAA242499697392D3, -183, -36), (0xFD87B5F28300CA0E, -157, -28), (0xBCE5086492111AEB, -130, -20),
    (0x8CBCCC096F5088CC, -103, -12), (0xD1B71758E219652C, -77, -4), (0x9C40000000000000, -50, 4),
    (0xE8D4A51000000000, -24, 12), (0xAD78EBC5AC620000, 3, 20), (0x813F3978F8940984, 30, 28),
    (0xC097CE7BC90715B3, 56, 36), (0x8F7E32CE7BEA5C70, 83, 44), (0xD5D238A4ABE98068, 109, 52),
    (0x9F4F2726179A2245, 136, 60), (0xED63A231D4C4FB27, 162, 68), (0xB0DE65388CC8ADA8, 189, 76),
    (0x83C7088E1AAB65DB, 216, 84), (0xC45D1DF942711D9A, 242, 92), (0x924D692CA61BE758, 269, 100),
    (0xDA01EE641A708DEA, 295, 108), (0xA26DA3999AEF774A, 322, 116), (0xF209787BB47D6B85, 348, 124),
    (0xB454E4A179DD1877, 375, 132), (0x865B86925B9BC5C2, 402, 140), (0xC83553C5C8965D3D, 428, 148),
    (0x952AB45CFA97A0B3, 455, 156), (0xDE469FBD99A05FE3, 481, 164), (0xA59BC234DB398C25, 508, 172),
    (0xF6C69A72A3989F5C, 534, 180), (0xB7DCBF5354E9BECE, 561, 188), (0x88FCF317F22241E2, 588, 196),
    (0xCC20CE9BD35C78A5, 614, 204), (0x98165AF37B2153DF, 641, 212), (0xE2A0B5DC971F303A, 667, 220),
    (0xA8D9D1535CE3B396, 694, 228), (0xFB9B7CD9A4A7443C, 720, 236), (0xBB764C4CA7A44410, 747, 244),
    (0x8BAB8EEFB6409C1A, 774, 252), (0xD01FEF10A657842C, 800, 260), (0x9B10A4E5E9913129, 827, 268),
    (0xE7109BFBA19C0C9D, 853, 276), (0xAC2820D9623BF429, 880, 284), (0x80444B5E7AA7CF85, 907, 292),
    (0xBF21E44003ACDD2D, 933, 300), (0x8E679C2F5E44FF8F, 960, 308), (0xD433179D9C8CB841, 986, 316),
    (0x9E19DB92B4E31BA9, 1013, 324),
];

fn cached_power_for_binary_exponent(e: i32) -> CachedPower {
    const MIN_DEC_EXP: i32 = -300;
    const DEC_STEP: i32 = 8;
    debug_assert!((-1500..=1500).contains(&e));
    let f = ALPHA - e - 1;
    // C++ integer division truncates towards zero, as Rust's does.
    let k = (f * 78913) / (1 << 18) + i32::from(f > 0);
    let index = (-MIN_DEC_EXP + k + (DEC_STEP - 1)) / DEC_STEP;
    let (f, e, k) = CACHED_POWERS[index as usize];
    CachedPower { f, e, k }
}

/// For n != 0 returns k with 10^(k-1) <= n < 10^k; for n == 0 returns 1 and pow10 = 1.
fn find_largest_pow10(n: u32) -> (i32, u32) {
    let mut k = 10;
    let mut pow10 = 1_000_000_000;
    while k > 1 && n < pow10 {
        k -= 1;
        pow10 /= 10;
    }
    (k, pow10)
}

fn grisu2_round(buf: &mut [u8], len: usize, dist: u64, delta: u64, mut rest: u64, ten_k: u64) {
    while rest < dist
        && delta - rest >= ten_k
        && (rest + ten_k < dist || dist - rest > rest + ten_k - dist)
    {
        buf[len - 1] -= 1;
        rest += ten_k;
    }
}

fn grisu2_digit_gen(
    buffer: &mut [u8],
    length: &mut usize,
    decimal_exponent: &mut i32,
    m_minus: DiyFp,
    w: DiyFp,
    m_plus: DiyFp,
) {
    let mut delta = DiyFp::sub(m_plus, m_minus).f;
    let mut dist = DiyFp::sub(m_plus, w).f;
    let shift = -m_plus.e;
    let one_f = 1u64 << shift;
    let mut p1 = (m_plus.f >> shift) as u32;
    let mut p2 = m_plus.f & (one_f - 1);

    let (k, mut pow10) = find_largest_pow10(p1);
    let mut n = k;
    while n > 0 {
        let d = p1 / pow10;
        let r = p1 % pow10;
        buffer[*length] = b'0' + d as u8;
        *length += 1;
        p1 = r;
        n -= 1;
        let rest = (u64::from(p1) << shift) + p2;
        if rest <= delta {
            *decimal_exponent += n;
            let ten_n = u64::from(pow10) << shift;
            grisu2_round(buffer, *length, dist, delta, rest, ten_n);
            return;
        }
        pow10 /= 10;
    }

    let mut m = 0;
    loop {
        p2 *= 10;
        let d = p2 >> shift;
        let r = p2 & (one_f - 1);
        buffer[*length] = b'0' + d as u8;
        *length += 1;
        p2 = r;
        m += 1;
        // nlohmann's unsigned arithmetic; Loitsch's bounds keep these from wrapping.
        delta = delta.wrapping_mul(10);
        dist = dist.wrapping_mul(10);
        if p2 <= delta {
            break;
        }
    }
    *decimal_exponent -= m;
    grisu2_round(buffer, *length, dist, delta, p2, one_f);
}

/// v = digits * 10^decimal_exponent for a finite positive value.
fn grisu2(buf: &mut [u8], value: f64) -> (usize, i32) {
    let b = compute_boundaries(value);
    let cached = cached_power_for_binary_exponent(b.plus.e);
    let c_minus_k = DiyFp {
        f: cached.f,
        e: cached.e,
    };
    let w = DiyFp::mul(b.w, c_minus_k);
    let w_minus = DiyFp::mul(b.minus, c_minus_k);
    let w_plus = DiyFp::mul(b.plus, c_minus_k);
    let m_minus = DiyFp {
        f: w_minus.f + 1,
        e: w_minus.e,
    };
    let m_plus = DiyFp {
        f: w_plus.f - 1,
        e: w_plus.e,
    };
    let mut len = 0;
    let mut decimal_exponent = -cached.k;
    grisu2_digit_gen(buf, &mut len, &mut decimal_exponent, m_minus, w, m_plus);
    (len, decimal_exponent)
}

fn append_exponent(out: &mut String, mut e: i32) {
    if e < 0 {
        e = -e;
        out.push('-');
    } else {
        out.push('+');
    }
    let k = e as u32;
    if k < 10 {
        out.push('0');
    }
    out.push_str(&k.to_string());
}

/// Fixed notation for 10^min_exp <= v < 10^max_exp, exponent notation otherwise.
fn format_buffer(
    out: &mut String,
    digits: &[u8],
    decimal_exponent: i32,
    min_exp: i32,
    max_exp: i32,
) {
    let k = digits.len() as i32;
    let n = k + decimal_exponent;
    let digits = std::str::from_utf8(digits).expect("ASCII digits");
    if k <= n && n <= max_exp {
        out.push_str(digits);
        out.extend(std::iter::repeat_n('0', (n - k) as usize));
        out.push_str(".0");
    } else if 0 < n && n <= max_exp {
        out.push_str(&digits[..n as usize]);
        out.push('.');
        out.push_str(&digits[n as usize..]);
    } else if min_exp < n && n <= 0 {
        out.push_str("0.");
        out.extend(std::iter::repeat_n('0', (-n) as usize));
        out.push_str(digits);
    } else {
        out.push_str(&digits[..1]);
        if k > 1 {
            out.push('.');
            out.push_str(&digits[1..]);
        }
        out.push('e');
        append_exponent(out, n - 1);
    }
}

/// Appends nlohmann's representation of a finite double (`detail::to_chars<double>`).
pub(crate) fn write_f64(out: &mut String, mut value: f64) {
    debug_assert!(value.is_finite());
    if value.is_sign_negative() {
        value = -value;
        out.push('-');
    }
    if value == 0.0 {
        out.push_str("0.0");
        return;
    }
    let mut buf = [0u8; 32];
    let (len, decimal_exponent) = grisu2(&mut buf, value);
    // kMinExp = -4, kMaxExp = std::numeric_limits<double>::digits10 = 15.
    format_buffer(out, &buf[..len], decimal_exponent, -4, 15);
}
