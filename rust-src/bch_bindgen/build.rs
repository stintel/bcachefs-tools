fn main() {
	use std::path::PathBuf;

	let out_dir: PathBuf = std::env::var_os("OUT_DIR").expect("ENV Var 'OUT_DIR' Expected").into();
	let top_dir: PathBuf = std::env::var_os("CARGO_MANIFEST_DIR")
		.expect("ENV Var 'CARGO_MANIFEST_DIR' Expected")
		.into();
	let libbcachefs_inc_dir =
		std::env::var("LIBBCACHEFS_INCLUDE").unwrap_or_else(|_| top_dir.join("libbcachefs").display().to_string());
	let libbcachefs_inc_dir = std::path::Path::new(&libbcachefs_inc_dir);
	println!("{}", libbcachefs_inc_dir.display());

	println!("cargo:rustc-link-lib=dylib=bcachefs");
	println!("cargo:rustc-link-search={}", env!("LIBBCACHEFS_LIB"));

	let _libbcachefs_dir = top_dir.join("libbcachefs").join("libbcachefs");
	let bindings = bindgen::builder()
		.header(top_dir.join("src").join("libbcachefs_wrapper.h").display().to_string())
		.clang_arg(format!("-I{}", libbcachefs_inc_dir.join("include").display()))
		.clang_arg(format!("-I{}", libbcachefs_inc_dir.display()))
		.clang_arg("-DZSTD_STATIC_LINKING_ONLY")
		.clang_arg("-DNO_BCACHEFS_FS")
		.clang_arg("-D_GNU_SOURCE")
		.derive_debug(true)
		.derive_default(true)
		.derive_eq(true)
		.layout_tests(true)
		.default_enum_style(bindgen::EnumVariation::Rust { non_exhaustive: true })
		.allowlist_function(".*bch2_.*")
		.allowlist_function("bio_.*")
		.allowlist_function("bch2_super_write_fd")
		.blocklist_type("bch_extent_ptr")
		.blocklist_type("btree_node")
		.blocklist_type("bch_extent_crc32")
		.blocklist_type("rhash_lock_head")
		.blocklist_type("srcu_struct")
		.allowlist_var("BCH_.*")
		.allowlist_type("bch_sb_field_.*")
		.opaque_type("gendisk")
		.opaque_type("bkey")
		.opaque_type("gc_stripe")
		.opaque_type("open_bucket.*")
		.generate()
		.expect("BindGen Generation Failiure: [libbcachefs_wrapper]");
	bindings
		.write_to_file(out_dir.join("bcachefs.rs"))
		.expect("Writing to output file failed for: `bcachefs.rs`");
}
