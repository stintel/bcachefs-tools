use std::ffi::CStr;
use std::ffi::CString;

use libc::c_char;
use rpassword;
use zeroize::Zeroize;

#[no_mangle]
pub extern fn free_cstring(s: *mut c_char) {
    unsafe {
        if s.is_null() {
            return;
        }
        drop(CString::from_raw(s));
    };
}

#[no_mangle]
pub extern fn read_passphrase(prompt: *const c_char) -> *mut c_char {
    let prompt_c_str: &CStr = unsafe { CStr::from_ptr(prompt) };
    let prompt_slice: &str = prompt_c_str.to_str().unwrap();
    let mut r_passphrase = rpassword::prompt_password(prompt_slice).unwrap();
    let c_passphrase = CString::new(r_passphrase.clone()).unwrap();
    r_passphrase.zeroize();
    c_passphrase.into_raw()
}
