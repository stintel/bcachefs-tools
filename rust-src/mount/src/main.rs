
use clap::Parser;

fn main() {
	// convert existing log statements to tracing events
	// tracing_log::LogTracer::init().expect("logtracer init failed!");
	// format tracing log data to env_logger like stdout
	tracing_subscriber::fmt::init();

	if let Err(e) = crate::main_inner() {
		tracing::error!(fatal_error = ?e);
	}
}


#[tracing_attributes::instrument("main")]
pub fn main_inner() -> anyhow::Result<()> {
	use bcachefs_mount::{Cli, filesystem};
	unsafe {
		libc::setvbuf(
			filesystem::stdout,
			std::ptr::null_mut(),
			libc::_IONBF,
			0,
		);
		// libc::fflush(filesystem::stdout);
	}

	let opt = Cli::parse();

	tracing::trace!(?opt);

	let fss = filesystem::probe_filesystems()?;
	let fs = fss
		.get(&opt.uuid)
		.ok_or_else(|| anyhow::anyhow!("filesystem was not found"))?;

	tracing::info!(msg="found filesystem", %fs);

	let mountpoint = opt
		.mountpoint
		.ok_or_else(|| anyhow::anyhow!("mountpoint option was not specified"))?;

	fs.mount(&mountpoint, &opt.options)?;

	Ok(())
}

#[cfg(test)]
mod test {
	// use insta::assert_debug_snapshot;
	// #[test]
	// fn snapshot_testing() {
	// 	insta::assert_debug_snapshot!();
	// }
}
