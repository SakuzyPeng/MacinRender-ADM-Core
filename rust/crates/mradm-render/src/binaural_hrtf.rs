//! HRTF preparation for the binaural renderers (PCM consistency 4.7 A), moved from the C++
//! `load_sofa_dataset` / `build_binaural_state`, the C++ geometry cache (`hrtf_grid.cpp`) and the
//! Scene stream renderer's HRTF state cache with its HRIR resampling, with the same checks, texts,
//! cache budgets and checkpoints. The prepared state is immutable and shared.
use crate::routing::Error;
use mradm_dsp::dataset::Dataset;
use mradm_dsp::diagnostics::{ENABLED, Words, process};
use mradm_dsp::hrtf::Grid;
use mradm_dsp::hrtf_filters::Filters;
use mradm_dsp::resampler::Resampler;
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

/// A user SOFA file: the UTF-8 path, display text and file name supplied by the C++ caller.
/// Unix paths retain their native bytes; Windows paths never pass through the ANSI code page.
#[derive(Clone, Copy, Debug)]
pub struct Sofa<'a> {
    pub path: &'a Path,
    pub display: &'a [u8],
    pub file_name: &'a [u8],
}

/// Prepared frequency-domain HRTFs and interpolation geometry. The HRIRs and measurement
/// directions are kept for the offline SAF spreader; the Scene cache drops them.
pub struct State {
    pub name: String,
    pub sample_rate: u32,
    pub num_dirs: usize,
    pub hrir_len: usize,
    pub fft_size: usize,
    pub overlap: usize,
    pub filters: Arc<Filters>,
    pub grid: Arc<Grid>,
    pub hrirs: Vec<f32>,
    pub directions: Vec<f32>,
}

fn error(code: i32, message: impl Into<Vec<u8>>, context: impl Into<Vec<u8>>) -> Error {
    Error {
        code,
        message: message.into(),
        context: context.into(),
    }
}

fn path_context(sofa: &Sofa) -> Vec<u8> {
    [b"path=".as_slice(), sofa.display].concat()
}

fn dsp(e: &mradm_dsp::Error, context: &[u8]) -> Error {
    error(crate::offline::dsp_code(e), e.to_string(), context)
}

/// The built-in KEMAR set or a user SOFA file. `input_rate` 0 accepts any SOFA rate; otherwise a
/// different rate is unsupported (the file renderer does not resample).
pub fn load(sofa: Option<&Sofa>, input_rate: u32) -> Result<Dataset, Error> {
    let Some(sofa) = sofa else {
        return Ok(Dataset::kemar());
    };
    if !cfg!(feature = "sofa") {
        return Err(error(
            2,
            "SOFA loading is disabled in this build (MR_ADM_ENABLE_SOFA=OFF)",
            path_context(sofa),
        ));
    }
    let bytes = read(sofa)?;
    let mut dataset = Dataset::sofa(&bytes).map_err(|e| dsp(&e, b""))?;
    if dataset.num_dirs > i32::MAX as usize || dataset.ir_len > i32::MAX as usize {
        return Err(error(2, "HRTF dataset exceeds renderer dimensions", ""));
    }
    if input_rate != 0 && input_rate != dataset.sample_rate {
        return Err(error(
            2,
            format!(
                "SOFA: sample rate {} Hz does not match input {} Hz",
                dataset.sample_rate, input_rate
            ),
            path_context(sofa),
        ));
    }
    // The C++ reader copied the name into a 256-byte C string.
    let mut n = dataset.name.len().min(255);
    while !dataset.name.is_char_boundary(n) {
        n -= 1;
    }
    let name = dataset.name[..n].split('\0').next().unwrap_or("");
    let name = if name.is_empty() {
        String::from_utf8_lossy(sofa.file_name).into_owned()
    } else {
        name.to_owned()
    };
    dataset.name = format!("SOFA {name}");
    Ok(dataset)
}

fn read(sofa: &Sofa) -> Result<Vec<u8>, Error> {
    use std::io::{Read, Seek, SeekFrom};
    let mut file = std::fs::File::open(sofa.path)
        .map_err(|_| error(3, "SOFA file could not be opened", path_context(sofa)))?;
    let length = file.seek(SeekFrom::End(0)).unwrap_or(0);
    if length == 0 || length > i64::MAX as u64 {
        return Err(error(3, "SOFA file is empty or unreadable", ""));
    }
    let incomplete = || error(3, "SOFA file could not be read completely", "");
    let length = usize::try_from(length).map_err(|_| incomplete())?;
    let mut bytes = Vec::new();
    bytes.try_reserve_exact(length).map_err(|_| incomplete())?;
    bytes.resize(length, 0);
    file.seek(SeekFrom::Start(0)).map_err(|_| incomplete())?;
    file.read_exact(&mut bytes).map_err(|_| incomplete())?;
    Ok(bytes)
}

/// Prepares the filter bank for OLA blocks of `block` frames, as the C++
/// `build_binaural_state`; `cache_magnitudes` selects the live bank layout.
pub fn build(dataset: Dataset, block: u64, cache_magnitudes: bool) -> Result<State, Error> {
    const MAX_FFT: u64 = 1 << 24;
    let len = dataset.ir_len as u64;
    if dataset.num_dirs == 0
        || dataset.num_dirs > i32::MAX as usize
        || dataset.ir_len == 0
        || dataset.ir_len > i32::MAX as usize
        || block == 0
        || block > MAX_FFT
        || len > MAX_FFT - block + 1
        || dataset.directions.len() != dataset.num_dirs * 2
    {
        return Err(error(1, "invalid HRTF preparation dimensions", ""));
    }
    let fft_size = (block + len - 1).next_power_of_two() as usize;
    let grid = prepare_grid(&dataset.directions)
        .ok_or_else(|| error(4, "HRTF direction triangulation failed", ""))?;
    // The C++ wrapper checked the impulse count before Rust validated the impulses.
    if dataset.num_dirs.checked_mul(2 * dataset.ir_len) != Some(dataset.impulses.len()) {
        return Err(error(
            1,
            "Invalid HRTF preparation arguments",
            "HRTF filters",
        ));
    }
    let filters = Filters::new(
        Arc::clone(&grid),
        &dataset.impulses,
        dataset.ir_len,
        fft_size,
        cache_magnitudes,
    )
    .map_err(|e| dsp(&e, b"HRTF filters"))?;
    if ENABLED {
        let mut spectrum = vec![0.0; filters.spectrum_len()];
        filters
            .copy_spectra(&mut spectrum)
            .map_err(|e| dsp(&e, b"HRTF spectrum snapshot"))?;
        let _ = process("binaural.01-hrir.f32", Words::F32(&dataset.impulses));
        let _ = process("binaural.02-grid.f32", Words::F32(&dataset.directions));
        let _ = process("binaural.03-hrtf.c32", Words::F32(&spectrum));
        let _ = process("binaural.04-vbap-gains.f32", Words::F32(grid.weights()));
        let _ = process("binaural.05-vbap-dirs.i32", Words::I32(grid.indices()));
    }
    Ok(State {
        name: dataset.name,
        sample_rate: dataset.sample_rate,
        num_dirs: dataset.num_dirs,
        hrir_len: dataset.ir_len,
        fft_size,
        overlap: dataset.ir_len - 1,
        filters: Arc::new(filters),
        grid,
        hrirs: dataset.impulses,
        directions: dataset.directions,
    })
}

// ── Geometry cache ────────────────────────────────────────────────────────────

const GRID_BUDGET: usize = 16 * 1024 * 1024;
const GRID_ENTRIES: usize = 8;
// The C++ cache entry (hash, key vector, shared pointer, size) and `dsp::HrtfGrid` (owning
// handle, info) on a 64-bit target.
const GRID_ENTRY_BYTES: usize = 56;
const GRID_WRAPPER_BYTES: usize = 40;

struct GridEntry {
    hash: u64,
    key: Vec<f32>,
    grid: Arc<Grid>,
    bytes: usize,
}

#[derive(Default)]
struct GridCache {
    entries: VecDeque<GridEntry>,
    bytes: usize,
}

impl GridCache {
    fn find(&mut self, key: &[f32], hash: u64) -> Option<Arc<Grid>> {
        let index = self.entries.iter().position(|e| {
            e.hash == hash
                && e.key.len() == key.len()
                && e.key
                    .iter()
                    .zip(key)
                    .all(|(a, b)| a.to_bits() == b.to_bits())
        })?;
        let entry = self.entries.remove(index)?;
        let grid = Arc::clone(&entry.grid);
        self.entries.push_back(entry);
        Some(grid)
    }
}

static GRIDS: Mutex<GridCache> = Mutex::new(GridCache {
    entries: VecDeque::new(),
    bytes: 0,
});
// Serialises cold builds, coalescing concurrent misses; no renderer or audio callback takes it.
static GRID_BUILD: Mutex<()> = Mutex::new(());

fn grids() -> std::sync::MutexGuard<'static, GridCache> {
    GRIDS.lock().unwrap_or_else(|e| e.into_inner())
}

/// The shared interpolation geometry for measurement directions; `None` when they cannot be
/// triangulated. Geometry does not depend on the HRIRs, FFT size or design rate.
pub fn prepare_grid(directions: &[f32]) -> Option<Arc<Grid>> {
    let mut hash = 14695981039346656037u64;
    for value in directions {
        hash ^= u64::from(value.to_bits());
        hash = hash.wrapping_mul(1099511628211);
    }
    if let Some(grid) = grids().find(directions, hash) {
        return Some(grid);
    }
    let _build = GRID_BUILD.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(grid) = grids().find(directions, hash) {
        return Some(grid);
    }
    let grid = Arc::new(Grid::new(directions).ok()?);
    let key = directions.to_vec();
    let bytes = GRID_ENTRY_BYTES
        + key.capacity() * size_of::<f32>()
        + GRID_WRAPPER_BYTES
        + grid.storage_bytes()
        + size_of::<Arc<Grid>>();
    if bytes <= GRID_BUDGET {
        let mut store = grids();
        while !store.entries.is_empty()
            && (store.entries.len() >= GRID_ENTRIES || store.bytes + bytes > GRID_BUDGET)
        {
            let front = store.entries.pop_front().unwrap();
            store.bytes -= front.bytes;
        }
        store.entries.push_back(GridEntry {
            hash,
            key,
            grid: Arc::clone(&grid),
            bytes,
        });
        store.bytes += bytes;
    }
    Some(grid)
}

/// Memory currently retained by the geometry cache.
pub fn grid_cache_bytes() -> usize {
    grids().bytes
}

// ── Scene stream HRTF state ───────────────────────────────────────────────────

/// The Scene stream renderer's OLA block.
pub const LIVE_BLOCK: u64 = 1024;
const STATE_BUDGET: usize = 64 * 1024 * 1024;
const STATE_ENTRIES: usize = 4;
// `sizeof(BinauralState)` and `sizeof(dsp::HrtfGrid)` of the C++ cache on a 64-bit target; the
// name counts at least the 15 bytes of its short-string buffer.
const STATE_BYTES: usize = 168 + GRID_WRAPPER_BYTES;

#[derive(Clone, PartialEq, Eq)]
struct Key {
    path: PathBuf,
    modified: Option<SystemTime>,
    size: u64,
    sample_rate: u32,
}

fn key(sofa: Option<&Sofa>, sample_rate: u32) -> Option<Key> {
    let mut key = Key {
        path: PathBuf::new(),
        modified: None,
        size: 0,
        sample_rate,
    };
    if let Some(sofa) = sofa {
        key.path = std::fs::canonicalize(sofa.path).ok()?;
        let metadata = std::fs::metadata(&key.path).ok()?;
        key.modified = Some(metadata.modified().ok()?);
        key.size = metadata.len();
    }
    Some(key)
}

struct StateEntry {
    key: Key,
    state: Arc<State>,
    bytes: usize,
}

struct StateCache {
    entries: Vec<StateEntry>,
    bytes: usize,
}

static STATES: Mutex<StateCache> = Mutex::new(StateCache {
    entries: Vec::new(),
    bytes: 0,
});

fn states() -> std::sync::MutexGuard<'static, StateCache> {
    STATES.lock().unwrap_or_else(|e| e.into_inner())
}

/// The Scene stream renderer's HRTF state at `sample_rate`, resampled when the set's rate
/// differs. Only immutable interpolation data is cached; file IO, resampling and preparation
/// never hold the cache lock.
pub fn prepare_live(sofa: Option<&Sofa>, sample_rate: u32) -> Result<Arc<State>, Error> {
    let found = key(sofa, sample_rate);
    if let Some(key) = &found {
        let mut cache = states();
        if let Some(index) = cache.entries.iter().position(|e| &e.key == key) {
            let entry = cache.entries.remove(index);
            let state = Arc::clone(&entry.state);
            cache.entries.push(entry);
            return Ok(state);
        }
    }
    let dataset = resample(load(sofa, 0)?, sample_rate)?;
    let mut state = build(dataset, LIVE_BLOCK, true)?;
    // Live convolution and extent rendering use only the spectra and the geometry.
    state.hrirs = Vec::new();
    state.directions = Vec::new();
    let state = Arc::new(state);
    if let Some(key) = found
        && Some(&key) == self::key(sofa, sample_rate).as_ref()
    {
        insert(key, Arc::clone(&state));
    }
    Ok(state)
}

fn insert(key: Key, state: Arc<State>) {
    let bytes = STATE_BYTES + state.name.len().max(15) + state.filters.storage_bytes();
    if bytes > STATE_BUDGET {
        return;
    }
    let mut cache = states();
    if cache.entries.iter().any(|e| e.key == key) {
        return;
    }
    while !cache.entries.is_empty()
        && (cache.entries.len() >= STATE_ENTRIES || cache.bytes + bytes > STATE_BUDGET)
    {
        let front = cache.entries.remove(0);
        cache.bytes -= front.bytes;
    }
    cache.entries.push(StateEntry { key, state, bytes });
    cache.bytes += bytes;
}

/// Resamples every HRIR to `sample_rate` (length rounded up), as the C++ `resample_dataset`.
pub fn resample(mut dataset: Dataset, sample_rate: u32) -> Result<Dataset, Error> {
    if dataset.sample_rate == sample_rate {
        return Ok(dataset);
    }
    if dataset.sample_rate == 0 || dataset.ir_len == 0 || sample_rate == 0 {
        return Err(error(
            4,
            "live binaural HRTF dataset has an invalid sample rate",
            "",
        ));
    }
    let input_length = dataset.ir_len;
    let numerator = dataset.ir_len as u64 * u64::from(sample_rate);
    let rate = u64::from(dataset.sample_rate);
    let output_frames = numerator.div_ceil(rate);
    let filters = dataset.num_dirs * 2;
    if output_frames > i32::MAX as u64
        || filters == 0
        || output_frames > (usize::MAX / filters) as u64
    {
        return Err(error(2, "resampled HRTF dataset is too large", ""));
    }
    let output_length = output_frames as usize;
    let resampler_error = |e: mradm_dsp::Error| dsp(&e, b"resampler");
    let mut resampler =
        Resampler::new(1, dataset.sample_rate, sample_rate).map_err(resampler_error)?;
    let mut output = vec![0.0; filters * output_length];
    for filter in 0..filters {
        resampler.reset();
        let source = &dataset.impulses[filter * input_length..(filter + 1) * input_length];
        let destination = &mut output[filter * output_length..(filter + 1) * output_length];
        let (mut consumed, mut written) = (0, 0);
        while consumed < input_length {
            let progress = resampler
                .process(&source[consumed..], &mut destination[written..])
                .map_err(resampler_error)?;
            consumed += progress.input_frames;
            written += progress.output_frames;
            if progress.input_frames == 0 && progress.output_frames == 0 {
                return Err(error(4, "HRTF resampler made no progress", ""));
            }
        }
        while written < output_length {
            let generated = resampler
                .finish(&mut destination[written..])
                .map_err(resampler_error)?;
            if generated == 0 {
                return Err(error(4, "HRTF resampler returned a short filter", ""));
            }
            written += generated;
        }
    }
    dataset.sample_rate = sample_rate;
    dataset.ir_len = output_length;
    dataset.impulses = output;
    Ok(dataset)
}

#[cfg(test)]
mod tests {
    use super::*;

    // The geometry cache is process-wide; tests that evict or compare entries take turns.
    static CACHE: Mutex<()> = Mutex::new(());
    fn serial() -> std::sync::MutexGuard<'static, ()> {
        CACHE.lock().unwrap_or_else(|e| e.into_inner())
    }

    #[test]
    fn geometry_is_cached_by_direction_bits_and_order() {
        let _serial = serial();
        let mut directions = vec![
            -135., 0., -45., 0., 45., 0., 135., 0., 0., 45., 90., 45., 180., 45., -90., 45.,
        ];
        let first = prepare_grid(&directions).unwrap();
        assert!(Arc::ptr_eq(&first, &prepare_grid(&directions).unwrap()));
        directions.swap(0, 2);
        directions.swap(1, 3);
        assert!(!Arc::ptr_eq(&first, &prepare_grid(&directions).unwrap()));
        for _ in 0..10 {
            directions[0] += 0.013;
            assert!(prepare_grid(&directions).is_some());
        }
        assert!(grid_cache_bytes() <= GRID_BUDGET);
        assert!(first.weights().iter().all(|w| w.is_finite()));
        directions[0] = f32::NAN;
        assert!(prepare_grid(&directions).is_none());
    }

    #[test]
    fn concurrent_misses_share_one_build() {
        let _serial = serial();
        let directions: Vec<f32> = vec![
            -130., 0., -40., 0., 50., 0., 140., 0., 0., 45., 90., 45., 180., 45., -90., 45.,
        ];
        let grids: Vec<_> = std::thread::scope(|s| {
            let tasks: Vec<_> = (0..4)
                .map(|_| s.spawn(|| prepare_grid(&directions).unwrap()))
                .collect();
            tasks.into_iter().map(|t| t.join().unwrap()).collect()
        });
        assert!(grids.iter().all(|g| Arc::ptr_eq(g, &grids[0])));
    }

    #[test]
    fn preparation_checks_dimensions() {
        let _serial = serial();
        let invalid = |e: Error| (e.code, String::from_utf8(e.message).unwrap());
        let empty = Dataset {
            sample_rate: 48000,
            num_dirs: 0,
            ir_len: 0,
            directions: Vec::new(),
            impulses: Vec::new(),
            name: String::new(),
        };
        assert_eq!(
            invalid(build(empty, 64, false).err().unwrap()),
            (1, "invalid HRTF preparation dimensions".into())
        );
        assert!(build(Dataset::kemar(), u64::MAX, false).is_err());
        let mut short = Dataset::kemar();
        short.impulses.pop();
        let e = build(short, 64, false).err().unwrap();
        assert_eq!(e.context, b"HRTF filters");
        let state = build(Dataset::kemar(), 1024, false).unwrap();
        assert_eq!((state.fft_size, state.overlap), (2048, 255));
        assert_eq!(state.name, "built-in KEMAR");
        let live = build(Dataset::kemar(), 1024, true).unwrap();
        assert!(
            Arc::ptr_eq(&state.grid, &live.grid),
            "offline and live banks share geometry"
        );
    }

    fn fixture(name: &str) -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../tests/fixtures/sofa")
            .join(name)
    }

    fn sofa_at(path: &Path) -> (PathBuf, Vec<u8>, Vec<u8>) {
        (
            path.to_path_buf(),
            path.to_string_lossy().into_owned().into_bytes(),
            path.file_name()
                .unwrap()
                .to_string_lossy()
                .into_owned()
                .into_bytes(),
        )
    }

    fn load_file(path: &Path, rate: u32) -> Result<Dataset, Error> {
        let (path, display, file_name) = sofa_at(path);
        load(
            Some(&Sofa {
                path: &path,
                display: &display,
                file_name: &file_name,
            }),
            rate,
        )
    }

    #[test]
    #[cfg(feature = "sofa")]
    fn sofa_files_keep_their_rate_name_and_samples() {
        for (file, rate) in [("simple.sofa", 48000), ("general-compressed.sofa", 44100)] {
            let data = load_file(&fixture(file), 0).unwrap();
            assert_eq!(
                (data.num_dirs, data.ir_len, data.sample_rate),
                (6, 16, rate)
            );
            assert_eq!(data.name, "SOFA MacinRender synthetic");
            for dir in 0..6 {
                assert_eq!(data.impulses[dir * 2 * 16 + 2], (dir + 1) as f32 / 8.);
                assert_eq!(data.impulses[(dir * 2 + 1) * 16 + 4], (6 - dir) as f32 / 8.);
            }
            assert!(build(data, 512, false).is_ok());
            let mismatch = load_file(&fixture(file), 96000).err().unwrap();
            assert_eq!(mismatch.code, 2);
            assert_eq!(
                mismatch.context,
                format!("path={}", fixture(file).display()).into_bytes()
            );
        }
        assert_eq!(
            load_file(&fixture("malformed.sofa"), 0).err().unwrap().code,
            3
        );
        let missing = load_file(&fixture("missing.sofa"), 0).err().unwrap();
        assert_eq!(
            (missing.code, missing.message.as_slice()),
            (3, b"SOFA file could not be opened".as_slice())
        );
    }

    #[test]
    #[cfg(not(feature = "sofa"))]
    fn sofa_off_is_unsupported() {
        assert_eq!(load_file(&fixture("simple.sofa"), 0).err().unwrap().code, 2);
    }

    #[test]
    fn resampling_rounds_the_length_up_and_keeps_equal_rates() {
        let kemar = Dataset::kemar();
        let same = resample(Dataset::kemar(), 48000).unwrap();
        assert_eq!(same.impulses, kemar.impulses);
        let down = resample(Dataset::kemar(), 44100).unwrap();
        assert_eq!((down.ir_len, down.sample_rate), (236, 44100));
        assert_eq!(down.impulses.len(), 836 * 2 * 236);
        assert_eq!(resample(Dataset::kemar(), 0).err().unwrap().code, 4);
    }

    #[test]
    fn live_cache_keys_follow_path_size_mtime_and_rate() {
        let dir = std::env::temp_dir().join(format!("mradm-hrtf-key-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("set.sofa");
        std::fs::write(&file, b"abcd").unwrap();
        let (path, display, file_name) = sofa_at(&file);
        let sofa = Sofa {
            path: &path,
            display: &display,
            file_name: &file_name,
        };
        let first = key(Some(&sofa), 48000).unwrap();
        assert!(first == key(Some(&sofa), 48000).unwrap());
        assert!(first != key(Some(&sofa), 44100).unwrap());
        assert!(first != key(None, 48000).unwrap());
        let modified = std::fs::metadata(&file).unwrap().modified().unwrap();
        // Same size and time: the C++ key did not look at the content either.
        std::fs::write(&file, b"wxyz").unwrap();
        std::fs::File::options()
            .write(true)
            .open(&file)
            .unwrap()
            .set_modified(modified)
            .unwrap();
        assert!(first == key(Some(&sofa), 48000).unwrap());
        std::fs::write(&file, b"abcde").unwrap();
        std::fs::File::options()
            .write(true)
            .open(&file)
            .unwrap()
            .set_modified(modified)
            .unwrap();
        assert!(
            first != key(Some(&sofa), 48000).unwrap(),
            "size is part of the key"
        );
        std::fs::write(&file, b"abcd").unwrap();
        assert!(
            first != key(Some(&sofa), 48000).unwrap(),
            "modification time is part of the key"
        );
        std::fs::remove_dir_all(&dir).unwrap();
        assert!(
            key(Some(&sofa), 48000).is_none(),
            "a missing file is not cached"
        );
    }

    #[test]
    fn live_states_are_cached_without_impulses() {
        let first = prepare_live(None, 32000).unwrap();
        assert!(first.hrirs.is_empty() && first.directions.is_empty());
        assert!(Arc::ptr_eq(&first, &prepare_live(None, 32000).unwrap()));
        assert_eq!(first.sample_rate, 32000);
    }
}
