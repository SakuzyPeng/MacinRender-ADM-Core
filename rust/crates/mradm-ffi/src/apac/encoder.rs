//! Synchronous AudioToolbox writer owned by the APAC worker.
use mradm_engine::request::Error;
use objc2_audio_toolbox as at;
use objc2_core_audio_types as ca;
use objc2_core_foundation::{CFArray, CFURL};
use std::mem::{ManuallyDrop, size_of_val};
use std::ptr::{self, NonNull};

fn failure(operation: &str, status: i32) -> Error {
    Error::new(3, format!("APAC: {operation} failed ({status})"), "")
}

fn check(status: i32, operation: &str) -> Result<(), Error> {
    if status == 0 {
        Ok(())
    } else {
        Err(failure(operation, status))
    }
}

pub(super) struct Writer {
    file: NonNull<at::OpaqueExtAudioFile>,
    channels: u32,
}

impl Writer {
    pub(super) fn open(
        output: &[u8],
        tag: ca::AudioChannelLayoutTag,
        bitrate_kbps: u32,
        drc_music: bool,
        caf: bool,
    ) -> Result<Self, Error> {
        // CoreFoundation copies the filesystem bytes and returns a retained URL.
        let url = unsafe {
            CFURL::from_file_system_representation(
                None,
                output.as_ptr(),
                output.len() as isize,
                false,
            )
        }
        .ok_or_else(|| Error::new(3, "APAC: CFURLCreate failed", ""))?;

        let channels = tag & 0xffff;
        let apac = ca::AudioStreamBasicDescription {
            mSampleRate: 48_000.,
            mFormatID: ca::kAudioFormatAPAC,
            mFormatFlags: 0,
            mBytesPerPacket: 0,
            mFramesPerPacket: 0,
            mBytesPerFrame: 0,
            mChannelsPerFrame: channels,
            mBitsPerChannel: 0,
            mReserved: 0,
        };
        let layout = ca::AudioChannelLayout {
            mChannelLayoutTag: tag,
            // A tagged layout has no channel bitmap or descriptions. All fields are numeric.
            ..unsafe { std::mem::zeroed() }
        };
        let container = if caf {
            at::kAudioFileCAFType
        } else {
            at::kAudioFileMPEG4Type
        };
        let mut raw = ptr::null_mut();
        // The format and layout are input records. A successful create transfers one file to us.
        let status = unsafe {
            at::ExtAudioFileCreateWithURL(
                &url,
                container,
                NonNull::from_ref(&apac),
                &layout,
                at::AudioFileFlags::EraseFile.bits(),
                NonNull::from_mut(&mut raw),
            )
        };
        check(status, "ExtAudioFileCreateWithURL")?;
        let writer = Self {
            file: NonNull::new(raw).ok_or_else(|| failure("ExtAudioFileCreateWithURL", status))?,
            channels,
        };

        let bytes_per_frame = size_of::<f32>() as u32 * channels;
        let pcm = ca::AudioStreamBasicDescription {
            mFormatID: ca::kAudioFormatLinearPCM,
            mFormatFlags: ca::kAudioFormatFlagIsFloat | ca::kAudioFormatFlagIsPacked,
            mBytesPerPacket: bytes_per_frame,
            mFramesPerPacket: 1,
            mBytesPerFrame: bytes_per_frame,
            mBitsPerChannel: 32,
            ..apac
        };
        // ClientDataFormat copies the ASBD. AudioConverter returns a file-owned pointer.
        let status = unsafe {
            at::ExtAudioFileSetProperty(
                raw,
                at::kExtAudioFileProperty_ClientDataFormat,
                size_of_val(&pcm) as u32,
                NonNull::from_ref(&pcm).cast(),
            )
        };
        check(status, "ClientDataFormat")?;
        let mut converter: at::AudioConverterRef = ptr::null_mut();
        let mut size = size_of_val(&converter) as u32;
        let status = unsafe {
            at::ExtAudioFileGetProperty(
                raw,
                at::kExtAudioFileProperty_AudioConverter,
                NonNull::from_mut(&mut size),
                NonNull::from_mut(&mut converter).cast(),
            )
        };
        if status != 0 || converter.is_null() {
            return Err(failure("AudioConverter lookup", status));
        }
        // Each listed converter property takes one 32-bit value, copied during the call.
        let set = |property, value: u32, operation| {
            let status = unsafe {
                at::AudioConverterSetProperty(
                    converter,
                    property,
                    size_of_val(&value) as u32,
                    NonNull::from_ref(&value).cast(),
                )
            };
            check(status, operation)
        };
        if bitrate_kbps != 0 {
            set(
                at::kAudioConverterEncodeBitRate,
                bitrate_kbps.wrapping_mul(1000),
                "EncodeBitRate",
            )?;
        }
        set(
            at::kAudioCodecPropertyContentSource,
            at::kAudioCodecContentSource_AppleAV_Spatial_Offline as u32,
            "csrc property",
        )?;
        set(
            at::kAudioCodecPropertyDynamicRangeControlConfiguration,
            u32::from(drc_music),
            "cdrc property",
        )?;
        set(at::kAudioCodecPropertyASPFrequency, 75, "aspf property")?;

        // ConverterConfig expects the address of a CFArrayRef. An empty array commits the changes.
        let empty = unsafe { CFArray::new(None, ptr::null_mut(), 0, ptr::null()) }
            .ok_or_else(|| Error::new(3, "APAC: CFArrayCreate failed (OOM?)", ""))?;
        let config: *const CFArray = &*empty;
        let status = unsafe {
            at::ExtAudioFileSetProperty(
                raw,
                at::kExtAudioFileProperty_ConverterConfig,
                size_of_val(&config) as u32,
                NonNull::from_ref(&config).cast(),
            )
        };
        check(status, "ConverterConfig commit")?;
        Ok(writer)
    }

    pub(super) fn write(&mut self, pcm: &mut [f32]) -> Result<(), Error> {
        let bytes = u32::try_from(size_of_val(pcm))
            .map_err(|_| Error::new(1, "APAC: PCM block is too large", ""))?;
        let frames = pcm.len() as u32 / self.channels;
        let mut buffers = ca::AudioBufferList {
            mNumberBuffers: 1,
            mBuffers: [ca::AudioBuffer {
                mNumberChannels: self.channels,
                mDataByteSize: bytes,
                mData: pcm.as_mut_ptr().cast(),
            }],
        };
        // ExtAudioFileWrite consumes these interleaved samples synchronously.
        let status = unsafe {
            at::ExtAudioFileWrite(self.file.as_ptr(), frames, NonNull::from_mut(&mut buffers))
        };
        check(status, "ExtAudioFileWrite")
    }

    pub(super) fn finish(self) -> Result<(), Error> {
        // Dispose consumes the file even when the final flush reports an error.
        let writer = ManuallyDrop::new(self);
        check(
            unsafe { at::ExtAudioFileDispose(writer.file.as_ptr()) },
            "ExtAudioFileDispose",
        )
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        // Covers configuration and write errors after a successful create.
        unsafe { at::ExtAudioFileDispose(self.file.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn create_failure_reports_the_audio_toolbox_status() {
        // A regular file as the parent makes creation fail on the native filesystem.
        let path = std::env::current_exe().unwrap().join("out.m4a");
        let error = Writer::open(
            path.as_os_str().as_encoded_bytes(),
            ca::kAudioChannelLayoutTag_Stereo,
            0,
            true,
            false,
        )
        .err()
        .expect("creating a file below the test executable must fail");
        assert_eq!(error.code, 3);
        let status = error
            .message
            .strip_prefix("APAC: ExtAudioFileCreateWithURL failed (")
            .and_then(|s| s.strip_suffix(')'))
            .unwrap()
            .parse::<i32>()
            .unwrap();
        assert_ne!(status, 0);
    }
}
