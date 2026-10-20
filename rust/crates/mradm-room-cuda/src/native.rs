//! Unsafe calls are confined to this private, versioned native plugin boundary.
use mradm_dsp::{
    Complex32, Error, Result,
    room::{
        gpu_experiment::{Backend, Factory, Frame, Job, LateBackend, Range, Task, Update},
        hrir::{HrirBank, PHASES},
        prepared::PreparedRoom,
        quality::Quality,
    },
};
use std::{
    collections::BTreeMap,
    ffi::{CStr, OsString, c_void},
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::{Path, PathBuf},
    sync::{Arc, Mutex, OnceLock, Weak},
};

const SOURCE: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../scripts/research/room_gpu/session_kernels.cu"
));
const ABI: u32 = 1;
#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryExW(path: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
    fn GetModuleHandleExW(flags: u32, address: *const u16, module: *mut *mut c_void) -> i32;
    fn GetModuleFileNameW(module: *mut c_void, path: *mut u16, size: u32) -> u32;
    fn AddDllDirectory(path: *const u16) -> *mut c_void;
    fn RemoveDllDirectory(cookie: *mut c_void) -> i32;
}
fn module_anchor() {}
fn default_library() -> Result<PathBuf> {
    let mut module = std::ptr::null_mut();
    if unsafe {
        GetModuleHandleExW(
            0x4 | 0x2,
            module_anchor as *const () as *const u16,
            &mut module,
        )
    } == 0
    {
        return Err(Error::Io("Cannot locate the ROOM CUDA host module"));
    }
    let mut path = vec![0u16; 32768];
    let used = unsafe { GetModuleFileNameW(module, path.as_mut_ptr(), path.len() as u32) } as usize;
    if used == 0 || used >= path.len() {
        return Err(Error::Io("Cannot locate the ROOM CUDA host directory"));
    }
    let path = PathBuf::from(OsString::from_wide(&path[..used]));
    Ok(path
        .parent()
        .ok_or(Error::Io("Missing ROOM CUDA host directory"))?
        .join("mradm_room_cuda.dll"))
}
struct ModuleHandle(*mut c_void);
impl Drop for ModuleHandle {
    fn drop(&mut self) {
        unsafe {
            FreeLibrary(self.0);
        }
    }
}
struct Directory(*mut c_void);
impl Drop for Directory {
    fn drop(&mut self) {
        unsafe {
            RemoveDllDirectory(self.0);
        }
    }
}

#[repr(C)]
struct Params {
    rate: u32,
    fft: u32,
    taps: u32,
    stride: u32,
    buckets: u32,
    rows: u32,
    images: u32,
    history_parts: u32,
    materials: *const f32,
    material_floats: usize,
    propagation: *const f32,
    propagation_floats: usize,
}
#[repr(C)]
struct NativeFrame {
    active: u32,
    reset: u32,
    heads: [u32; 2],
    frequency: *const f32,
    jobs: *const Job,
    job_count: usize,
    ranges: *const Range,
    range_count: usize,
    updates: *const Update,
    update_count: usize,
    tasks: *const Task,
    task_count: usize,
}
#[repr(C)]
struct BankDesc {
    abi: u32,
    size: u32,
    rate: u32,
    fft: u32,
    rows: u32,
}
#[derive(Default, Debug)]
#[repr(C)]
pub struct Stats {
    pub banks: u64,
    pub engines: u64,
    pub late_engines: u64,
    pub device_bytes: u64,
    pub pinned_bytes: u64,
}
type Destroy = unsafe extern "C" fn(*mut c_void);
type BankCreate = unsafe extern "C" fn(*const BankDesc, *const u8, usize) -> *mut c_void;
type BankUpload = unsafe extern "C" fn(*mut c_void, u32, usize, *const f32, usize) -> i32;
type BankSeal = unsafe extern "C" fn(*mut c_void) -> i32;
type BankDevice = unsafe extern "C" fn(*mut c_void) -> *const std::ffi::c_char;
type StatsFn = unsafe extern "C" fn(*mut Stats) -> i32;
type Create = unsafe extern "C" fn(*mut c_void, *const Params) -> *mut c_void;
type Begin = unsafe extern "C" fn(*mut c_void, u32) -> i32;
type Voice = unsafe extern "C" fn(*mut c_void, u32, *const NativeFrame) -> i32;
type Submit = unsafe extern "C" fn(*mut c_void) -> i32;
type Finish = unsafe extern "C" fn(*mut c_void, *mut f32, usize) -> i32;
type Message = unsafe extern "C" fn() -> *const std::ffi::c_char;
type LateCreate = unsafe extern "C" fn(*mut c_void, u32, u32, *const f32, usize) -> *mut c_void;
type LateSubmit = unsafe extern "C" fn(*mut c_void, *const f32, usize, u32) -> i32;
struct Library {
    _module: ModuleHandle,
    _directory: Directory,
    bank_create: BankCreate,
    bank_upload: BankUpload,
    bank_seal: BankSeal,
    bank_destroy: Destroy,
    bank_device: BankDevice,
    stats: StatsFn,
    create: Create,
    destroy: Destroy,
    begin: Begin,
    voice: Voice,
    submit: Submit,
    finish: Finish,
    message: Message,
    late_create: LateCreate,
    late_destroy: Destroy,
    late_submit: LateSubmit,
    late_finish: Finish,
}
// Module ownership and immutable function pointers may cross preparation/worker threads.
unsafe impl Send for Library {}
unsafe impl Sync for Library {}
impl Library {
    fn load(path: &Path) -> Result<Arc<Self>> {
        let parent: Vec<_> = path
            .parent()
            .ok_or(Error::Io("Missing ROOM CUDA runtime directory"))?
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect();
        let directory = Directory(unsafe { AddDllDirectory(parent.as_ptr()) });
        if directory.0.is_null() {
            return Err(Error::Io("Cannot register ROOM CUDA runtime directory"));
        }
        let wide: Vec<_> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let module = ModuleHandle(unsafe {
            LoadLibraryExW(wide.as_ptr(), std::ptr::null_mut(), 0x100 | 0x1000)
        });
        if module.0.is_null() {
            return Err(Error::Unsupported(
                "Optional ROOM CUDA runtime or its dependencies are unavailable",
            ));
        }
        let symbol = |name: &[u8]| -> Result<*mut c_void> {
            let pointer = unsafe { GetProcAddress(module.0, name.as_ptr()) };
            if pointer.is_null() {
                Err(Error::Unsupported("Incompatible ROOM CUDA runtime"))
            } else {
                Ok(pointer)
            }
        };
        macro_rules! get {
            ($name:literal,$ty:ty) => {
                unsafe {
                    std::mem::transmute::<*mut c_void, $ty>(symbol(
                        concat!($name, "\0").as_bytes(),
                    )?)
                }
            };
        }
        let version = get!("mradm_room_cuda_abi", unsafe extern "C" fn() -> u32);
        if unsafe { version() } != ABI {
            return Err(Error::Unsupported("Incompatible ROOM CUDA ABI"));
        }
        let value = Self {
            bank_create: get!("mradm_room_cuda_bank_create", BankCreate),
            bank_upload: get!("mradm_room_cuda_bank_upload", BankUpload),
            bank_seal: get!("mradm_room_cuda_bank_seal", BankSeal),
            bank_destroy: get!("mradm_room_cuda_bank_destroy", Destroy),
            bank_device: get!("mradm_room_cuda_bank_device", BankDevice),
            stats: get!("mradm_room_cuda_stats", StatsFn),
            create: get!("mradm_room_cuda_session_create", Create),
            destroy: get!("mradm_room_cuda_session_destroy", Destroy),
            begin: get!("mradm_room_cuda_session_begin", Begin),
            voice: get!("mradm_room_cuda_session_voice", Voice),
            submit: get!("mradm_room_cuda_session_submit", Submit),
            finish: get!("mradm_room_cuda_session_finish", Finish),
            message: get!("mradm_room_cuda_session_error", Message),
            late_create: get!("mradm_room_cuda_late_create", LateCreate),
            late_destroy: get!("mradm_room_cuda_late_destroy", Destroy),
            late_submit: get!("mradm_room_cuda_late_submit", LateSubmit),
            late_finish: get!("mradm_room_cuda_late_finish", Finish),
            _module: module,
            _directory: directory,
        };
        Ok(Arc::new(value))
    }
    fn check(&self, status: i32) -> Result<()> {
        if status == 0 {
            return Ok(());
        }
        let message = unsafe { (self.message)() };
        if !message.is_null() {
            eprintln!(
                "ROOM CUDA: {}",
                unsafe { CStr::from_ptr(message) }.to_string_lossy()
            );
        }
        Err(Error::RenderFailed(
            "ROOM CUDA operation failed; recreate the session after correcting the device/runtime error",
        ))
    }
}
struct Bank {
    library: Arc<Library>,
    handle: *mut c_void,
    cpu: Arc<HrirBank>,
}
// Upload is complete before publication. Engines only read these shared tables.
unsafe impl Send for Bank {}
unsafe impl Sync for Bank {}
impl Drop for Bank {
    fn drop(&mut self) {
        unsafe {
            (self.library.bank_destroy)(self.handle);
        }
    }
}
impl Bank {
    fn new(cpu: Arc<HrirBank>, path: &Path) -> Result<Self> {
        let library = Library::load(path)?;
        let layers = cpu.data.collection.as_ref().map_or(1, |c| c.layers.len());
        let rows = layers
            .checked_mul(cpu.data.directions.len())
            .and_then(|n| n.checked_mul(2))
            .ok_or(Error::Unsupported("ROOM CUDA bank is too large"))?;
        if rows > 65536 {
            return Err(Error::Unsupported("ROOM CUDA bank is too large"));
        }
        let desc = BankDesc {
            abi: ABI,
            size: size_of::<BankDesc>() as u32,
            rate: cpu.data.sample_rate,
            fft: cpu.fft_length as u32,
            rows: rows as u32,
        };
        let handle = unsafe { (library.bank_create)(&desc, SOURCE.as_ptr(), SOURCE.len()) };
        if handle.is_null() {
            library.check(1)?;
        }
        let bank = Self {
            library,
            handle,
            cpu,
        };
        let bins = bank.cpu.fft_length / 2 + 1;
        let mut buffer = vec![0f32; 64 * bins * 2];
        for (table, total) in [(0, rows), (1, PHASES + 1)] {
            for first in (0..total).step_by(64) {
                let count = 64.min(total - first);
                let row = &mut buffer[..count * bins * 2];
                if table == 0 {
                    bank.cpu.gpu_copy_rows(first, row)?;
                } else {
                    bank.cpu.gpu_copy_fractions(first, row)?;
                }
                bank.library.check(unsafe {
                    (bank.library.bank_upload)(
                        handle,
                        table,
                        first * bins * 2,
                        row.as_ptr(),
                        row.len(),
                    )
                })?;
            }
        }
        let phases = &mut buffer[..bank.cpu.fft_length * 2];
        for (bin, out) in phases.as_chunks_mut::<2>().0.iter_mut().enumerate() {
            let v = bank.cpu.integer_phase(1, bin);
            out.copy_from_slice(&[v.re, v.im]);
        }
        bank.library.check(unsafe {
            (bank.library.bank_upload)(handle, 2, 0, phases.as_ptr(), phases.len())
        })?;
        bank.library
            .check(unsafe { (bank.library.bank_seal)(handle) })?;
        Ok(bank)
    }
}
type Entry = OnceLock<Result<Bank>>;
fn copy_error(error: &Error) -> Error {
    match error {
        Error::InvalidArgument(s) => Error::InvalidArgument(s),
        Error::Unsupported(s) => Error::Unsupported(s),
        Error::RenderFailed(s) => Error::RenderFailed(s),
        Error::Io(s) => Error::Io(s),
    }
}
pub struct NativeFactory {
    entry: Arc<Entry>,
}
impl NativeFactory {
    pub fn new(room: Arc<PreparedRoom>, library: Option<&Path>) -> Result<Arc<Self>> {
        if room.quality != Quality::Realtime
            || room.profile.sample_rate != 96000
            || room.fft_length != 4096
            || room.early_channels() != 2
        {
            return Err(Error::Unsupported(
                "This room/HRIR configuration is not supported by the optional Windows CUDA backend",
            ));
        }
        let path = library
            .map(Path::to_path_buf)
            .map_or_else(default_library, Ok)?;
        let path = path
            .canonicalize()
            .map_err(|_| Error::Unsupported("Optional ROOM CUDA runtime is unavailable"))?;
        type Cache = BTreeMap<(PathBuf, usize), Weak<Entry>>;
        static CACHE: OnceLock<Mutex<Cache>> = OnceLock::new();
        let key = (path.clone(), Arc::as_ptr(&room.hrir) as usize);
        let entry = {
            let mut entries = CACHE
                .get_or_init(Default::default)
                .lock()
                .map_err(|_| Error::RenderFailed("ROOM CUDA cache failed"))?;
            entries.retain(|_, v| v.strong_count() != 0);
            if let Some(value) = entries.get(&key).and_then(Weak::upgrade) {
                value
            } else {
                let value = Arc::new(OnceLock::new());
                entries.insert(key, Arc::downgrade(&value));
                value
            }
        };
        // Per-key single-flight initialization never holds the global cache lock.
        entry
            .get_or_init(|| Bank::new(room.hrir.clone(), &path))
            .as_ref()
            .map_err(copy_error)?;
        Ok(Arc::new(Self { entry }))
    }
    fn bank(&self) -> &Bank {
        self.entry.get().unwrap().as_ref().unwrap()
    }
    pub fn stats(&self) -> Result<Stats> {
        let mut value = Stats::default();
        self.bank()
            .library
            .check(unsafe { (self.bank().library.stats)(&mut value) })?;
        Ok(value)
    }
    pub fn metadata(&self) -> Result<serde_json::Value> {
        let bank = self.bank();
        let name = unsafe { (bank.library.bank_device)(bank.handle) };
        if name.is_null() {
            bank.library.check(1)?;
        }
        Ok(
            serde_json::json!({"kind":"cuda","abi":ABI,"device":unsafe { CStr::from_ptr(name) }.to_string_lossy(),"binding":"prepared-memory","kernel_sha256":mradm_dsp::room::resources::sha256(SOURCE.as_bytes()),"eq_stagger_frames":256,"maximum_sources":32}),
        )
    }
}

impl Factory for NativeFactory {
    fn create(&self, room: Arc<PreparedRoom>) -> Result<Box<dyn Backend>> {
        let bank = self.bank();
        if !Arc::ptr_eq(&room.hrir, &bank.cpu) || room.quality != Quality::Realtime {
            return Err(Error::InvalidArgument(
                "ROOM CUDA prepared resource binding changed",
            ));
        }
        let materials: Vec<_> = room
            .gpu_material()
            .iter()
            .flat_map(|v| [v.re, v.im])
            .collect();
        let bins = room.fft_length / 2 + 1;
        let mut propagation = Vec::with_capacity((PHASES + 1) * bins * 2);
        for phase in 0..=PHASES {
            propagation.extend(
                room.gpu_propagation_row(phase)?
                    .iter()
                    .flat_map(|v| [v.re, v.im]),
            );
        }
        let layers = room
            .hrir
            .data
            .collection
            .as_ref()
            .map_or(1, |c| c.layers.len());
        let params = Params {
            rate: room.profile.sample_rate,
            fft: room.fft_length as u32,
            taps: room.short_taps as u32,
            stride: room.path_bucket_stride as u32,
            buckets: room.short_taps.div_ceil(room.path_bucket_stride) as u32,
            rows: (layers * room.hrir.data.directions.len() * 2) as u32,
            images: room.images.len() as u32,
            history_parts: room.short_taps.max(room.send_taps).div_ceil(256) as u32,
            materials: materials.as_ptr(),
            material_floats: materials.len(),
            propagation: propagation.as_ptr(),
            propagation_floats: propagation.len(),
        };
        let context = unsafe { (bank.library.create)(bank.handle, &params) };
        if context.is_null() {
            bank.library.check(1)?;
        }
        Ok(Box::new(Native {
            _entry: self.entry.clone(),
            library: bank.library.clone(),
            context,
        }))
    }
    fn create_late(&self, room: Arc<PreparedRoom>) -> Result<Option<Box<dyn LateBackend>>> {
        let bank = self.bank();
        if !Arc::ptr_eq(&room.hrir, &bank.cpu) {
            return Err(Error::InvalidArgument(
                "ROOM CUDA late resource binding changed",
            ));
        }
        let Some(first) = room.late[0].gpu_tail() else {
            return Ok(None);
        };
        let parts = first.len() / 2049;
        let mut filters = Vec::with_capacity(first.len() * room.late.len() * 2);
        for row in &room.late {
            let values = row
                .gpu_tail()
                .ok_or(Error::Unsupported("Inconsistent ROOM CUDA late shape"))?;
            if row.channels != 1 || values.len() != first.len() {
                return Err(Error::Unsupported("Inconsistent ROOM CUDA late shape"));
            }
            filters.extend(values.iter().flat_map(|v| [v.re, v.im]));
        }
        let context = unsafe {
            (bank.library.late_create)(
                bank.handle,
                parts as u32,
                room.late.len() as u32,
                filters.as_ptr(),
                filters.len(),
            )
        };
        if context.is_null() {
            bank.library.check(1)?;
        }
        Ok(Some(Box::new(NativeLate {
            _entry: self.entry.clone(),
            library: bank.library.clone(),
            context,
        })))
    }
}
struct NativeLate {
    _entry: Arc<Entry>,
    library: Arc<Library>,
    context: *mut c_void,
}
// The owning WorldTail serializes submissions and consumes each result before reuse.
unsafe impl Send for NativeLate {}
impl Drop for NativeLate {
    fn drop(&mut self) {
        unsafe {
            (self.library.late_destroy)(self.context);
        }
    }
}
impl LateBackend for NativeLate {
    fn submit(&mut self, frequency: &[Complex32], reset: bool) -> Result<()> {
        if frequency.len() != 2049 {
            return Err(Error::InvalidArgument("Invalid GPU late input"));
        }
        let mut row = [0.; 4098];
        for (out, value) in row.as_chunks_mut::<2>().0.iter_mut().zip(frequency) {
            out.copy_from_slice(&[value.re, value.im]);
        }
        self.library.check(unsafe {
            (self.library.late_submit)(self.context, row.as_ptr(), row.len(), u32::from(reset))
        })
    }
    fn finish(&mut self, output: &mut [f32]) -> Result<()> {
        self.library.check(unsafe {
            (self.library.late_finish)(self.context, output.as_mut_ptr(), output.len())
        })
    }
}
struct Native {
    _entry: Arc<Entry>,
    library: Arc<Library>,
    context: *mut c_void,
}
// The Session owns this engine and serializes begin/submit/finish. The DLL
// binds the CUDA primary context when used on the persistent worker thread.
unsafe impl Send for Native {}
impl Drop for Native {
    fn drop(&mut self) {
        unsafe {
            (self.library.destroy)(self.context);
        }
    }
}
impl Backend for Native {
    fn begin(&mut self, count: usize) -> Result<()> {
        self.library
            .check(unsafe { (self.library.begin)(self.context, count as u32) })
    }
    fn voice(&mut self, index: usize, frame: &Frame) -> Result<()> {
        let mut updates = [Update::default(); 32];
        if frame.requests.len() > updates.len() {
            return Err(Error::Unsupported("GPU update capacity exceeded"));
        }
        for (out, request) in updates.iter_mut().zip(&frame.requests) {
            *out = request.update;
        }
        let f = NativeFrame {
            active: u32::from(frame.active),
            reset: u32::from(frame.reset),
            heads: frame.heads,
            frequency: frame.frequency.as_ptr(),
            jobs: frame.jobs.as_ptr(),
            job_count: frame.jobs.len(),
            ranges: frame.ranges.as_ptr(),
            range_count: frame.ranges.len(),
            updates: updates.as_ptr(),
            update_count: frame.requests.len(),
            tasks: frame.tasks.as_ptr(),
            task_count: frame.tasks.len(),
        };
        self.library
            .check(unsafe { (self.library.voice)(self.context, index as u32, &f) })
    }
    fn submit(&mut self) -> Result<()> {
        self.library
            .check(unsafe { (self.library.submit)(self.context) })
    }
    fn finish(&mut self, output: &mut [f32]) -> Result<()> {
        self.library.check(unsafe {
            (self.library.finish)(self.context, output.as_mut_ptr(), output.len())
        })
    }
}
