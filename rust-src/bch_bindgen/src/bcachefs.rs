#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(unused)]

include!(concat!(env!("OUT_DIR"), "/bcachefs.rs"));

use bitfield::bitfield;
use memoffset::offset_of;
impl PartialEq for bch_sb {
	fn eq(&self, other: &Self) -> bool {
		self.magic.b == other.magic.b
		&& self.user_uuid.b == other.user_uuid.b
		&& self.block_size == other.block_size
		&& self.version == other.version
		&& self.uuid.b == other.uuid.b
		&& self.seq == other.seq
	}
}

impl bch_sb {
	pub fn uuid(&self) -> uuid::Uuid {
		uuid::Uuid::from_bytes(self.user_uuid.b)
	}
}
impl bch_sb_handle {
	pub fn sb(&self) -> &bch_sb {
		unsafe { &*self.sb }
	}

	pub fn bdev(&self) -> &block_device {
		unsafe { &*self.bdev }
	}
}

#[repr(C)]
// #[repr(align(8))]
#[derive(Debug, Default, Copy, Clone)]
pub struct bch_extent_ptr {
	pub _bitfield_1: __BindgenBitfieldUnit<[u8; 8usize]>,
}

#[repr(C, packed(8))]
pub struct btree_node {
	pub csum: bch_csum,
	pub magic: __le64,
	pub flags: __le64,
	pub min_key: bpos,
	pub max_key: bpos,
	pub _ptr: bch_extent_ptr,
	pub format: bkey_format,
	pub __bindgen_anon_1: btree_node__bindgen_ty_1,
}

#[repr(C, packed(8))]
// #[repr(align(8))]
#[derive(Debug, Default, Copy, Clone)]
pub struct bch_extent_crc32 {
	pub _bitfield_1: __BindgenBitfieldUnit<[u8; 4usize]>,
	pub csum: __u32,
}

// #[repr(u8)]
pub enum rhash_lock_head {}
pub enum srcu_struct {}
