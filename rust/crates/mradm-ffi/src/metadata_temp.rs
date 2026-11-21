use std::collections::hash_map::RandomState;
use std::fs::{File, OpenOptions};
use std::hash::{BuildHasher, Hasher};
use std::io;
use std::path::{Path, PathBuf};

/// Stages a metadata rewrite beside the output and removes an unfinished rewrite on drop.
pub(crate) struct MetadataTemp {
    path: Option<PathBuf>,
    #[cfg(unix)]
    permissions: std::fs::Permissions,
}

impl MetadataTemp {
    pub(crate) fn create(output: &Path) -> io::Result<(Self, File)> {
        let mut options = OpenOptions::new();
        options.read(true).write(true).create_new(true);
        #[cfg(unix)]
        let permissions = {
            use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
            let permissions = output.metadata()?.permissions();
            options.mode(permissions.mode() & 0o600);
            permissions
        };
        let directory = output.parent().unwrap_or(Path::new(""));
        for _ in 0..16 {
            let nonce = RandomState::new().build_hasher().finish();
            let path = directory.join(format!(".mradm-metadata-{nonce:016x}.tmp"));
            match options.open(&path) {
                Ok(file) => {
                    return Ok((
                        Self {
                            path: Some(path),
                            #[cfg(unix)]
                            permissions,
                        },
                        file,
                    ));
                }
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
        Err(io::ErrorKind::AlreadyExists.into())
    }

    pub(crate) fn install(mut self, output: &Path) -> io::Result<()> {
        let path = self.path.as_ref().expect("staged metadata file");
        #[cfg(unix)]
        std::fs::set_permissions(path, self.permissions.clone())?;
        std::fs::rename(path, output)?;
        self.path = None;
        Ok(())
    }
}

impl Drop for MetadataTemp {
    fn drop(&mut self) {
        if let Some(path) = &self.path {
            let _ = std::fs::remove_file(path);
        }
    }
}
