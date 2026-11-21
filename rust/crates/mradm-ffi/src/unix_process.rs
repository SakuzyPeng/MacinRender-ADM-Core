//! Pipe and process-group operations shared by the MP4 packager and APAC watchdog.
use std::io::{self, PipeReader};
use std::os::fd::AsRawFd;
use std::process::Child;

pub(crate) fn set_nonblocking(reader: &PipeReader) -> io::Result<()> {
    let fd = reader.as_raw_fd();
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    if flags == -1 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } == -1 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

pub(crate) fn signal_group(child: &Child, signal: libc::c_int) {
    let pid = child.id() as libc::pid_t;
    if unsafe { libc::kill(-pid, signal) } != 0 {
        unsafe { libc::kill(pid, signal) };
    }
}
