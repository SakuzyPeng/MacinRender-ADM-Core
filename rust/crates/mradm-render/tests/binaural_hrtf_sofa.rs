//! SOFA preparation through non-ASCII paths (formerly the C++ `sofa_fixture_test`, R3 B): the file
//! and Scene preparations read the same set through a Unicode directory, the error context keeps
//! the UTF-8 path, and the file renderer's FFT layout follows the block size. The dataset contract
//! itself (dimensions, rate, name, unnormalized samples, rate mismatch, malformed and missing
//! files) is tested in `binaural_hrtf`'s unit tests.
use mradm_render::binaural_hrtf::{Sofa, build, load, prepare_live};
use std::path::{Path, PathBuf};

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../tests/fixtures/sofa")
        .join(name)
}

fn with_sofa<T>(path: &Path, f: impl FnOnce(&Sofa) -> T) -> T {
    let display = path.to_string_lossy().into_owned().into_bytes();
    let name = path
        .file_name()
        .unwrap()
        .to_string_lossy()
        .into_owned()
        .into_bytes();
    f(&Sofa {
        path,
        display: &display,
        file_name: &name,
    })
}

fn spectra(filters: &mradm_dsp::hrtf_filters::Filters) -> Vec<f32> {
    let mut out = vec![0f32; filters.spectrum_len()];
    filters.copy_spectra(&mut out).unwrap();
    out
}

struct TempDir(PathBuf);
impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[test]
fn unicode_paths_load_the_same_set_in_both_preparations() {
    let root = TempDir(std::env::temp_dir().join(format!("mradm-sofa-{}", std::process::id())));
    let nested = root.0.join("\u{6d4b}\u{8bd5}-\u{1f3a7}");
    std::fs::create_dir_all(&nested).unwrap();
    let path = nested.join("hrtf-\u{1f3a7}.sofa");
    std::fs::copy(fixture("simple.sofa"), &path).unwrap();

    let file = with_sofa(&path, |s| load(Some(s), 48000));
    let live = with_sofa(&path, |s| prepare_live(Some(s), 44100));
    if cfg!(feature = "sofa") {
        let (file, live) = (file.unwrap(), live.unwrap());
        let reference = with_sofa(&fixture("simple.sofa"), |s| load(Some(s), 48000)).unwrap();
        let reference_live =
            with_sofa(&fixture("simple.sofa"), |s| prepare_live(Some(s), 44100)).unwrap();
        assert_eq!(file.name, reference.name);
        assert_eq!(live.name, reference_live.name);
        let (state, expected) = (
            build(file, 512, false).unwrap(),
            build(reference, 512, false).unwrap(),
        );
        assert_eq!(spectra(&state.filters), spectra(&expected.filters));
        assert_eq!(spectra(&live.filters), spectra(&reference_live.filters));
        // The file renderer's layout of a 16-tap set at a 512-frame block.
        assert_eq!((state.num_dirs, state.hrir_len), (6, 16));
        assert_eq!((state.fft_size, state.overlap), (1024, 15));
    } else {
        assert_eq!(file.err().unwrap().code, 2);
        assert_eq!(live.err().unwrap().code, 2);
    }

    let missing = nested.join("missing-\u{1f3a7}.sofa");
    let error = with_sofa(&missing, |s| load(Some(s), 48000)).err().unwrap();
    assert_eq!(
        error.context,
        format!("path={}", missing.to_string_lossy()).into_bytes()
    );
}
