//! The only unsafe boundary for the independent CUDA DLL. All borrowed buffers
//! are copied by the DLL before return; GPU processing uses its own pinned data.
use mradm_dsp::{
    Complex32, Error, Result,
    room::{
        gpu_experiment::{Backend, Factory, Frame, Job, LateBackend, Range, Task, Update},
        prepared::PreparedRoom,
    },
};
use std::{
    ffi::{CStr, CString, c_void},
    path::{Path, PathBuf},
    sync::Arc,
};
#[link(name = "kernel32")]
unsafe extern "system" {
    fn LoadLibraryW(path: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
    fn FreeLibrary(module: *mut c_void) -> i32;
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
type Create = unsafe extern "C" fn(*const u8, *const u8, *const Params) -> *mut c_void;
type Destroy = unsafe extern "C" fn(*mut c_void);
type Begin = unsafe extern "C" fn(*mut c_void, u32) -> i32;
type Voice = unsafe extern "C" fn(*mut c_void, u32, *const NativeFrame) -> i32;
type Submit = unsafe extern "C" fn(*mut c_void) -> i32;
type Finish = unsafe extern "C" fn(*mut c_void, *mut f32, usize) -> i32;
type Message = unsafe extern "C" fn() -> *const std::ffi::c_char;
type LateCreate =
    unsafe extern "C" fn(*const u8, *const u8, u32, u32, *const f32, usize) -> *mut c_void;
type LateSubmit = unsafe extern "C" fn(*mut c_void, *const f32, usize, u32) -> i32;
struct Library {
    handle: *mut c_void,
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
// Immutable function pointers; each engine is serialized by the Session.
unsafe impl Send for Library {}
unsafe impl Sync for Library {}
impl Drop for Library {
    fn drop(&mut self) {
        unsafe {
            FreeLibrary(self.handle);
        }
    }
}
impl Library {
    fn load(path: &Path) -> std::result::Result<Self, Box<dyn std::error::Error>> {
        use std::os::windows::ffi::OsStrExt;
        let wide: Vec<_> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let handle = unsafe { LoadLibraryW(wide.as_ptr()) };
        if handle.is_null() {
            return Err(std::io::Error::last_os_error().into());
        }
        let symbol = |name: &[u8]| -> std::result::Result<*mut c_void, Box<dyn std::error::Error>> {
            let p = unsafe { GetProcAddress(handle, name.as_ptr()) };
            if p.is_null() {
                Err("Missing GPU DLL symbol".into())
            } else {
                Ok(p)
            }
        };
        Ok(Self {
            handle,
            create: unsafe {
                std::mem::transmute::<*mut c_void, Create>(symbol(b"room_gpu_session_create\0")?)
            },
            destroy: unsafe {
                std::mem::transmute::<*mut c_void, Destroy>(symbol(b"room_gpu_session_destroy\0")?)
            },
            begin: unsafe {
                std::mem::transmute::<*mut c_void, Begin>(symbol(b"room_gpu_session_begin\0")?)
            },
            voice: unsafe {
                std::mem::transmute::<*mut c_void, Voice>(symbol(b"room_gpu_session_voice\0")?)
            },
            submit: unsafe {
                std::mem::transmute::<*mut c_void, Submit>(symbol(b"room_gpu_session_submit\0")?)
            },
            finish: unsafe {
                std::mem::transmute::<*mut c_void, Finish>(symbol(b"room_gpu_session_finish\0")?)
            },
            message: unsafe {
                std::mem::transmute::<*mut c_void, Message>(symbol(b"room_gpu_session_error\0")?)
            },
            late_create: unsafe {
                std::mem::transmute::<*mut c_void, LateCreate>(symbol(b"room_gpu_late_create\0")?)
            },
            late_destroy: unsafe {
                std::mem::transmute::<*mut c_void, Destroy>(symbol(b"room_gpu_late_destroy\0")?)
            },
            late_submit: unsafe {
                std::mem::transmute::<*mut c_void, LateSubmit>(symbol(b"room_gpu_late_submit\0")?)
            },
            late_finish: unsafe {
                std::mem::transmute::<*mut c_void, Finish>(symbol(b"room_gpu_late_finish\0")?)
            },
        })
    }
    fn check(&self, status: i32) -> Result<()> {
        if status == 0 {
            Ok(())
        } else {
            let p = unsafe { (self.message)() };
            if !p.is_null() {
                eprintln!("GPU: {}", unsafe { CStr::from_ptr(p) }.to_string_lossy());
            }
            Err(Error::RenderFailed("GPU session backend failed"))
        }
    }
}
pub struct NativeFactory {
    library: Arc<Library>,
    fixture: PathBuf,
    kernels: PathBuf,
    hrir_hash: String,
    fft: usize,
}
impl NativeFactory {
    pub fn new(
        library: &Path,
        fixture: &Path,
        kernels: &Path,
    ) -> std::result::Result<Self, Box<dyn std::error::Error>> {
        let manifest: serde_json::Value =
            serde_json::from_slice(&std::fs::read(fixture.join("manifest.json"))?)?;
        Ok(Self {
            library: Arc::new(Library::load(library)?),
            fixture: fixture.to_owned(),
            kernels: kernels.to_owned(),
            hrir_hash: manifest["hrir_hash"]
                .as_str()
                .ok_or("Missing GPU HRIR identity")?
                .into(),
            fft: manifest["fft_length"]
                .as_u64()
                .ok_or("Missing GPU FFT size")? as usize,
        })
    }
}
impl Factory for NativeFactory {
    fn create_late(&self, room: Arc<PreparedRoom>) -> Result<Option<Box<dyn LateBackend>>> {
        let Some(first) = room.late[0].gpu_tail() else {
            return Ok(None);
        };
        let parts = first.len() / 2049;
        let mut filters = Vec::with_capacity(first.len() * room.late.len() * 2);
        for row in &room.late {
            let tail = row
                .gpu_tail()
                .ok_or(Error::Unsupported("Inconsistent GPU late shapes"))?;
            if row.channels != 1 || tail.len() != first.len() {
                return Err(Error::Unsupported("Inconsistent GPU late shapes"));
            }
            filters.extend(tail.iter().flat_map(|v| [v.re, v.im]));
        }
        let path = |p: &Path| {
            CString::new(p.to_string_lossy().as_bytes())
                .map_err(|_| Error::InvalidArgument("Invalid GPU path"))
        };
        let fixture = path(&self.fixture)?;
        let kernels = path(&self.kernels)?;
        let context = unsafe {
            (self.library.late_create)(
                fixture.as_ptr().cast(),
                kernels.as_ptr().cast(),
                parts as u32,
                room.late.len() as u32,
                filters.as_ptr(),
                filters.len(),
            )
        };
        if context.is_null() {
            self.library.check(1)?;
        }
        Ok(Some(Box::new(NativeLate {
            library: self.library.clone(),
            context,
        })))
    }
    fn create(&self, room: Arc<PreparedRoom>) -> Result<Box<dyn Backend>> {
        if room.hrir.data.identity != self.hrir_hash || room.fft_length != self.fft {
            return Err(Error::InvalidArgument("GPU fixture HRIR/FFT mismatch"));
        }
        let (materials, propagation) = super::tables(&room)?;
        let layers = room
            .hrir
            .data
            .collection
            .as_ref()
            .map_or(1, |v| v.layers.len());
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
        let path = |p: &Path| {
            CString::new(p.to_string_lossy().as_bytes())
                .map_err(|_| Error::InvalidArgument("Invalid GPU path"))
        };
        let fixture = path(&self.fixture)?;
        let kernels = path(&self.kernels)?;
        let context = unsafe {
            (self.library.create)(fixture.as_ptr().cast(), kernels.as_ptr().cast(), &params)
        };
        if context.is_null() {
            self.library.check(1)?;
        }
        Ok(Box::new(Native {
            library: self.library.clone(),
            context,
        }))
    }
}
struct NativeLate {
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
