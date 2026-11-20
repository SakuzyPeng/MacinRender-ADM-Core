use std::collections::hash_map::RandomState;
use std::fs::File;
use std::hash::{BuildHasher, Hasher};
use std::io;
use std::path::{Path, PathBuf};

/// Stages a metadata rewrite beside the output and removes an unfinished rewrite on drop.
pub(crate) struct MetadataTemp(Option<PathBuf>);

impl MetadataTemp {
    pub(crate) fn create(output: &Path) -> io::Result<(Self, File)> {
        let directory = output.parent().unwrap_or(Path::new(""));
        for _ in 0..16 {
            let nonce = RandomState::new().build_hasher().finish();
            let path = directory.join(format!(".mradm-metadata-{nonce:016x}.tmp"));
            match File::create_new(&path) {
                Ok(file) => return Ok((Self(Some(path)), file)),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
        Err(io::ErrorKind::AlreadyExists.into())
    }

    pub(crate) fn install(mut self, output: &Path) -> io::Result<()> {
        std::fs::rename(self.0.as_ref().expect("staged metadata file"), output)?;
        self.0 = None;
        Ok(())
    }
}

impl Drop for MetadataTemp {
    fn drop(&mut self) {
        if let Some(path) = &self.0 {
            let _ = std::fs::remove_file(path);
        }
    }
}
