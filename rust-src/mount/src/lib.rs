use anyhow::anyhow;
use clap::Parser;
use uuid::Uuid;

pub mod err {
	pub enum GError {
		Unknown{
			message: std::borrow::Cow<'static, String>
		 }
	}
	pub type GResult<T, E, OE> =::core::result::Result< ::core::result::Result<T, E>, OE>;
	pub type Result<T, E> = GResult<T, E, GError>;
}

#[macro_export]
macro_rules! c_str {
	($lit:expr) => {
		unsafe {
			std::ffi::CStr::from_ptr(concat!($lit, "\0").as_ptr() as *const std::os::raw::c_char)
				.to_bytes_with_nul()
				.as_ptr() as *const std::os::raw::c_char
		}
	};
}

#[derive(Debug)]
struct ErrnoError(errno::Errno);
impl std::fmt::Display for ErrnoError {
	fn fmt(&self, f: &mut std::fmt::Formatter) -> Result<(), std::fmt::Error> {
		self.0.fmt(f)
	}
}
impl std::error::Error for ErrnoError {}

#[derive(Clone, Debug)]
pub enum KeyLocation {
	Fail,
	Wait,
	Ask,
}

#[derive(Clone, Debug)]
pub struct KeyLoc(pub Option<KeyLocation>);
impl std::ops::Deref for KeyLoc {
	type Target = Option<KeyLocation>;
	fn deref(&self) -> &Self::Target {
		&self.0
	}
}
impl std::str::FromStr for KeyLoc {
	type Err = anyhow::Error;
	fn from_str(s: &str) -> anyhow::Result<Self> {
		// use anyhow::anyhow;
		match s {
			"" => Ok(KeyLoc(None)),
			"fail" => Ok(KeyLoc(Some(KeyLocation::Fail))),
			"wait" => Ok(KeyLoc(Some(KeyLocation::Wait))),
			"ask" => Ok(KeyLoc(Some(KeyLocation::Ask))),
			_ => Err(anyhow!("invalid password option")),
		}
	}
}

fn parse_fstab_uuid(uuid_raw: &str) -> Result<Uuid, uuid::Error> {
	let mut uuid = String::from(uuid_raw);
	if uuid.starts_with("UUID=") {
		uuid = uuid.replacen("UUID=", "", 1);
	}
	return Uuid::parse_str(&uuid);
}

/// Mount a bcachefs filesystem by its UUID.
#[derive(Parser, Debug)]
#[command(author, version, about, long_about = None)]
pub struct Cli {
	/// External UUID of the bcachefs filesystem
	///
	/// Accepts the UUID as is or as fstab style UUID=<UUID>
	#[arg(value_parser=parse_fstab_uuid)]
	pub uuid: uuid::Uuid,

	/// Where the filesystem should be mounted. If not set, then the filesystem
	/// won't actually be mounted. But all steps preceeding mounting the
	/// filesystem (e.g. asking for passphrase) will still be performed.
	pub mountpoint: Option<std::path::PathBuf>,

	/// Mount options
	#[arg(short, default_value = "")]
	pub options: String,
}

pub mod filesystem;

// pub fn mnt_in_use()

#[test]
fn verify_cli() {
	use clap::CommandFactory;
	Cli::command().debug_assert()
}
