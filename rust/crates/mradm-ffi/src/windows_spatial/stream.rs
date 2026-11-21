//! All COM interfaces live on the render thread between CoInitializeEx and CoUninitialize.
use super::bed::Bed;
use mradm_engine::request::{Error, code};
use std::{
    mem::{ManuallyDrop, MaybeUninit},
    ptr, slice,
};
use windows::Win32::{
    Foundation::{E_UNEXPECTED, HANDLE},
    Media::{Audio::*, Multimedia::WAVE_FORMAT_IEEE_FLOAT},
    System::{
        Com::{StructuredStorage::*, *},
        Threading::ResetEvent,
        Variant::VT_BLOB,
    },
};
use windows::core::{Error as WinError, Result as WinResult};

pub(super) const RENDER_FAILED: i32 = 4;

fn hr(code: i32, message: &str, e: WinError) -> Error {
    Error::new(
        code,
        message,
        format!("system-spatial hr=0x{:08X}", e.code().0 as u32),
    )
}

pub(super) struct Apartment;
impl Apartment {
    pub fn enter() -> Result<Self, Error> {
        // SAFETY: this dedicated thread owns one MTA initialization, including S_FALSE.
        unsafe { CoInitializeEx(None, COINIT_MULTITHREADED).ok() }
            .map_err(|e| hr(RENDER_FAILED, "初始化空间音频 COM 失败", e))?;
        Ok(Self)
    }
}
impl Drop for Apartment {
    fn drop(&mut self) {
        unsafe { CoUninitialize() };
    }
}

pub(super) struct Stream {
    objects: Vec<Option<ISpatialAudioObject>>,
    stream: ISpatialAudioObjectRenderStream,
    _client: ISpatialAudioClient,
    scratch: Vec<f32>,
}

impl Stream {
    pub fn open(bed: &Bed, rate: u32, event: HANDLE) -> Result<Self, Error> {
        // SAFETY: the caller owns this thread's COM apartment and keeps the event
        // open until the stream and its objects are released.
        let enumerator: IMMDeviceEnumerator =
            unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) }
                .map_err(|e| hr(RENDER_FAILED, "创建 MMDeviceEnumerator 失败", e))?;
        let endpoint = unsafe { enumerator.GetDefaultAudioEndpoint(eRender, eConsole) }
            .map_err(|e| hr(RENDER_FAILED, "获取默认音频输出端点失败", e))?;
        let client: ISpatialAudioClient = unsafe { endpoint.Activate(CLSCTX_INPROC_SERVER, None) }
            .map_err(|e| hr(code::UNSUPPORTED, "当前输出设备不支持空间音频(请在 Windows 声音设置中启用 Windows Sonic / Dolby Atmos / DTS 头戴空间音频)", e))?;

        let format = WAVEFORMATEX {
            wFormatTag: WAVE_FORMAT_IEEE_FLOAT as u16,
            nChannels: 1,
            nSamplesPerSec: rate,
            nAvgBytesPerSec: rate.saturating_mul(4),
            nBlockAlign: 4,
            wBitsPerSample: 32,
            cbSize: 0,
        };
        unsafe { client.IsAudioObjectFormatSupported(&format) }.map_err(|e| {
            Error::new(
                code::UNSUPPORTED,
                "系统空间音频不支持该对象格式(float mono)",
                format!(
                    "system-spatial sample_rate={rate}Hz hr=0x{:08X}",
                    e.code().0 as u32
                ),
            )
        })?;
        unsafe { ResetEvent(event) }
            .map_err(|e| hr(RENDER_FAILED, "重置空间音频缓冲事件失败", e))?;
        let mut params = SpatialAudioObjectRenderStreamActivationParams {
            ObjectFormat: &format,
            StaticObjectTypeMask: bed.mask,
            MinDynamicObjectCount: bed.dynamic,
            MaxDynamicObjectCount: bed.dynamic,
            Category: AudioCategory_Media,
            EventHandle: event,
            ..Default::default()
        };
        // VT_BLOB borrows the stack params and format for the synchronous activation.
        // The binding's PROPVARIANT destructor would free the borrowed blob with CoTaskMemFree.
        let activation = ManuallyDrop::new(PROPVARIANT {
            Anonymous: PROPVARIANT_0 {
                Anonymous: ManuallyDrop::new(PROPVARIANT_0_0 {
                    vt: VT_BLOB,
                    Anonymous: PROPVARIANT_0_0_0 {
                        blob: BLOB {
                            cbSize: size_of_val(&params) as u32,
                            pBlobData: ptr::from_mut(&mut params).cast(),
                        },
                    },
                    ..Default::default()
                }),
            },
        });
        let stream = unsafe { client.ActivateSpatialAudioStream(&*activation) }.map_err(|e| {
            hr(
                code::UNSUPPORTED,
                "激活空间音频流失败(可能未启用空间音频格式)",
                e,
            )
        })?;
        let max_frames = unsafe { client.GetMaxFrameCount(&format) }
            .map_err(|e| hr(RENDER_FAILED, "获取空间音频最大缓冲帧数失败", e))?;
        let samples = (max_frames as usize)
            .checked_mul(bed.channels)
            .filter(|&n| n != 0)
            .ok_or_else(|| {
                Error::new(RENDER_FAILED, "空间音频最大缓冲帧数无效", "system-spatial")
            })?;
        let result = Self {
            objects: vec![None; bed.routes.len()],
            stream,
            _client: client,
            scratch: vec![0.0; samples],
        };
        unsafe { result.stream.Start() }.map_err(|e| hr(RENDER_FAILED, "启动空间音频流失败", e))?;
        Ok(result)
    }

    pub fn update(
        &mut self,
        bed: &Bed,
        pull: &mut impl FnMut(&mut [f32], usize) -> usize,
    ) -> WinResult<()> {
        let mut frames = 0;
        unsafe { self.stream.BeginUpdatingAudioObjects(&mut 0, &mut frames) }?;
        // End every successful Begin, including an empty block, a bad size, or a failed object.
        let filled = self.fill(bed, frames as usize, pull);
        let ended = unsafe { self.stream.EndUpdatingAudioObjects() };
        filled.and(ended)
    }

    fn fill(
        &mut self,
        bed: &Bed,
        frames: usize,
        pull: &mut impl FnMut(&mut [f32], usize) -> usize,
    ) -> WinResult<()> {
        if frames == 0 {
            return Ok(());
        }
        let pcm = self
            .scratch
            .get_mut(..frames.checked_mul(bed.channels).ok_or(E_UNEXPECTED)?)
            .ok_or(E_UNEXPECTED)?;
        let produced = pull(pcm, frames).min(frames);
        pcm[produced * bed.channels..].fill(0.0);
        for (slot, route) in self.objects.iter_mut().zip(&bed.routes) {
            let object = match slot {
                Some(object) => object,
                None => slot.insert(unsafe { self.stream.ActivateSpatialAudioObject(route.kind) }?),
            };
            if let Some([x, y, z]) = route.position {
                // Some endpoints forget the coordinates unless set on every update.
                unsafe { object.SetPosition(x, y, z) }?;
            }
            let (mut data, mut bytes) = (ptr::null_mut(), 0);
            unsafe { object.GetBuffer(&mut data, &mut bytes) }?;
            let samples = data.cast::<MaybeUninit<f32>>();
            if samples.is_null() || !samples.is_aligned() {
                return Err(E_UNEXPECTED.into());
            }
            let count = frames.min(bytes as usize / size_of::<f32>());
            // SAFETY: GetBuffer grants exclusive write access for this update. The negotiated
            // format is mono float, and the slice ends before EndUpdatingAudioObjects.
            let output = unsafe { slice::from_raw_parts_mut(samples, count) };
            route.write(pcm, bed.channels, output);
        }
        Ok(())
    }
}
impl Drop for Stream {
    fn drop(&mut self) {
        unsafe {
            let _ = self.stream.Stop();
            let _ = self.stream.Reset();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::Wake;
    use super::*;
    use mradm_scene::layouts::Geometry;
    use windows::Win32::{Foundation::WAIT_OBJECT_0, System::Threading::WaitForSingleObject};

    #[test]
    #[ignore = "requires a Windows default output endpoint with spatial sound enabled"]
    fn rejected_block_ends_its_update_and_the_next_short_pull_is_padded() {
        let _apartment = Apartment::enter().unwrap();
        let bed = Bed::new("4+7+0", 12, Geometry::Standard).unwrap();
        let wake = Wake::new().unwrap();
        let mut stream = Stream::open(&bed, 48000, wake.handle()).unwrap();
        let mut scratch = std::mem::take(&mut stream.scratch);
        let until = std::time::Instant::now() + std::time::Duration::from_secs(2);
        let wait = || {
            assert!(std::time::Instant::now() < until);
            assert_eq!(
                unsafe { WaitForSingleObject(wake.handle(), 1000) },
                WAIT_OBJECT_0
            );
        };
        // A successful Begin may report zero frames while the endpoint starts.
        let error = loop {
            wait();
            if let Err(e) =
                stream.update(&bed, &mut |_, _| panic!("an oversized block must not pull"))
            {
                break e;
            }
        };
        assert_eq!(error.code(), E_UNEXPECTED);
        scratch.fill(0.5);
        stream.scratch = scratch;
        let mut filled = 0;
        while filled < 2 * bed.channels {
            wait();
            stream
                .update(&bed, &mut |out, _| {
                    out[..bed.channels].fill(0.0);
                    filled = out.len();
                    1
                })
                .unwrap();
        }
        assert!(stream.scratch[..filled].iter().all(|&sample| sample == 0.0));
    }
}
