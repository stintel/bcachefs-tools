extern "C" {
	pub static stdout: *mut libc::FILE;
}

use getset::{CopyGetters, Getters};
use std::path::PathBuf;
#[derive(Getters, CopyGetters)]
pub struct FileSystem {
	/// External UUID of the bcachefs
	#[getset(get = "pub")]
	uuid: uuid::Uuid,
	/// Super block
	#[getset(get = "pub")]
	sb: bcachefs::bch_sb_handle,
	/// Member devices for this filesystem
	#[getset(get = "pub")]
	devices: Vec<PathBuf>,
}
impl std::fmt::Debug for FileSystem {
	fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
		f.debug_struct("FileSystem")
			.field("uuid", &self.uuid)
			.field("devices", &self.device_string())
			.finish()
	}
}
use std::fmt;
impl std::fmt::Display for FileSystem {
	fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
		let devs = self.device_string();
		write!(
			f,
			"{:?}: ({}) ",
			self.uuid,
			devs,
		)
	}
}

impl FileSystem {
	pub(crate) fn new(sb: bcachefs::bch_sb_handle) -> Self {
		Self {
			uuid: sb.sb().uuid(),
			sb: sb,
			devices: Vec::new(),
		}
	}

	pub fn device_string(&self) -> String {
		use itertools::Itertools;
		self.devices.iter().map(|d| d.display()).join(":")
	}

	pub fn mount(
		&self,
		target: impl AsRef<std::path::Path>,
		options: impl AsRef<str>,
	) -> anyhow::Result<()> {
		tracing::info_span!("mount").in_scope(|| {
			let src = self.device_string();
			let (data, mountflags) = parse_mount_options(options);
			// let fstype = c_str!("bcachefs");

			tracing::info!(msg="mounting bcachefs filesystem", target=%target.as_ref().display());
			mount_inner(src, target, "bcachefs", mountflags, data)
		})
	}
}

fn mount_inner(
	src: String,
	target: impl AsRef<std::path::Path>,
	fstype: &str,
	mountflags: u64,
	data: Option<String>,
) -> anyhow::Result<()> {
	use std::{
		ffi::{c_void, CString},
		os::{raw::c_char, unix::ffi::OsStrExt},
	};

	// bind the CStrings to keep them alive
	let src = CString::new(src)?;
	let target = CString::new(target.as_ref().as_os_str().as_bytes())?;
	let data = data.map(CString::new).transpose()?;
	let fstype = CString::new(fstype)?;

	// convert to pointers for ffi
	let src = src.as_c_str().to_bytes_with_nul().as_ptr() as *const c_char;
	let target = target.as_c_str().to_bytes_with_nul().as_ptr() as *const c_char;
	let data = data.as_ref().map_or(std::ptr::null(), |data| {
		data.as_c_str().to_bytes_with_nul().as_ptr() as *const c_void
	});
	let fstype = fstype.as_c_str().to_bytes_with_nul().as_ptr() as *const c_char;
	
	let ret = {let _entered = tracing::info_span!("libc::mount").entered();
		tracing::info!("mounting filesystem");
		// REQUIRES: CAP_SYS_ADMIN
		unsafe { libc::mount(src, target, fstype, mountflags, data) }
	};
	match ret {
		0 => Ok(()),
		_ => Err(crate::ErrnoError(errno::errno()).into()),
	}
}

/// Parse a comma-separated mount options and split out mountflags and filesystem
/// specific options.
#[tracing_attributes::instrument(skip(options))]
fn parse_mount_options(options: impl AsRef<str>) -> (Option<String>, u64) {
	use either::Either::*;
	tracing::debug!(msg="parsing mount options", options=?options.as_ref());
	let (opts, flags) = options
		.as_ref()
		.split(",")
		.map(|o| match o {
			"dirsync" => Left(libc::MS_DIRSYNC),
			"lazytime" => Left(1 << 25), // MS_LAZYTIME
			"mand" => Left(libc::MS_MANDLOCK),
			"noatime" => Left(libc::MS_NOATIME),
			"nodev" => Left(libc::MS_NODEV),
			"nodiratime" => Left(libc::MS_NODIRATIME),
			"noexec" => Left(libc::MS_NOEXEC),
			"nosuid" => Left(libc::MS_NOSUID),
			"ro" => Left(libc::MS_RDONLY),
			"rw" => Left(0),
			"relatime" => Left(libc::MS_RELATIME),
			"strictatime" => Left(libc::MS_STRICTATIME),
			"sync" => Left(libc::MS_SYNCHRONOUS),
			"" => Left(0),
			o @ _ => Right(o),
		})
		.fold((Vec::new(), 0), |(mut opts, flags), next| match next {
			Left(f) => (opts, flags | f),
			Right(o) => {
				opts.push(o);
				(opts, flags)
			}
		});

	use itertools::Itertools;
	(
		if opts.len() == 0 {
			None
		} else {
			Some(opts.iter().join(","))
		},
		flags,
	)
}

use bch_bindgen::bcachefs;
use std::collections::HashMap;
use uuid::Uuid;

#[tracing_attributes::instrument]
pub fn probe_filesystems() -> anyhow::Result<HashMap<Uuid, FileSystem>> {
	tracing::trace!("enumerating udev devices");
	let mut udev = udev::Enumerator::new()?;

	udev.match_subsystem("block")?; // find kernel block devices

	let mut fs_map = HashMap::new();
	let devresults = 
			udev.scan_devices()?
			.into_iter()
			.filter_map(|dev| dev.devnode().map(ToOwned::to_owned));
	
	for pathbuf in devresults {
		match get_super_block_uuid(&pathbuf)? {

				Ok((uuid_key, superblock)) => {
					let fs = fs_map.entry(uuid_key).or_insert_with(|| {
						tracing::info!(msg="found bcachefs pool", uuid=?uuid_key);
						FileSystem::new(superblock)
					});

					fs.devices.push(pathbuf);
				},

				Err(e) => { tracing::debug!(inner2_error=?e);}
		}
	}

	
	tracing::info!(msg = "found filesystems", count = fs_map.len());
	Ok(fs_map)
}

// #[tracing_attributes::instrument(skip(dev, fs_map))]
fn get_super_block_uuid(path: &std::path::Path) -> std::io::Result<std::io::Result<(Uuid, bcachefs::bch_sb_handle)>> {
	use gag::{BufferRedirect};
	// Stop libbcachefs from spamming the output
	let gag = BufferRedirect::stdout().unwrap();

	let sb = bch_bindgen::rs::read_super(&path)?;
	let super_block = match sb { 
		Err(e) => { return Ok(Err(e)); }
		Ok(sb) => sb,
	};
	drop(gag);

	let uuid = (&super_block).sb().uuid();
	tracing::debug!(found="bcachefs superblock", devnode=?path, ?uuid);

	Ok(Ok((uuid, super_block)))
}
