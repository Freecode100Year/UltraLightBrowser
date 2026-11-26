use std::marker::PhantomData;
use std::mem::{offset_of, size_of};
use std::os::raw::c_int;
use std::sync::atomic::{AtomicUsize, Ordering};

#[allow(
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    clippy::all
)]
pub mod sys {
    include!(concat!(env!("OUT_DIR"), "/cef_bindings.rs"));
}

/// Owned UTF-16 `cef_string_t` wrapper that cleans up memory via `cef_string_utf16_clear`.
pub struct CefStr {
    raw: sys::cef_string_t,
}

impl CefStr {
    pub fn from_str(s: &str) -> Self {
        let mut raw = sys::cef_string_t::default();
        let utf16: Vec<u16> = s.encode_utf16().collect();
        if !utf16.is_empty() {
            unsafe {
                sys::cef_string_utf16_set(utf16.as_ptr(), utf16.len(), &mut raw, 1);
            }
        }
        Self { raw }
    }

    pub fn as_ptr(&self) -> *const sys::cef_string_t {
        &self.raw
    }

    /// Transfers ownership into an existing out-parameter `cef_string_t` (e.g. `CefSettings`).
    pub fn write_into(s: &str, out: &mut sys::cef_string_t) {
        let utf16: Vec<u16> = s.encode_utf16().collect();
        if !utf16.is_empty() {
            unsafe {
                sys::cef_string_utf16_set(utf16.as_ptr(), utf16.len(), out, 1);
            }
        }
    }
}

impl Drop for CefStr {
    fn drop(&mut self) {
        unsafe {
            sys::cef_string_utf16_clear(&mut self.raw);
        }
    }
}

/// Decodes a borrowed `*const cef_string_t` into a Rust `String`.
pub unsafe fn read_cef_str(s: *const sys::cef_string_t) -> String {
    if s.is_null() {
        return String::new();
    }
    let r = &*s;
    if r.str_.is_null() || r.length == 0 {
        return String::new();
    }
    let slice = std::slice::from_raw_parts(r.str_, r.length);
    String::from_utf16_lossy(slice)
}

/// Decodes and frees a `cef_string_userfree_t` returned by a CEF getter.
pub unsafe fn take_userfree_str(s: sys::cef_string_userfree_t) -> String {
    if s.is_null() {
        return String::new();
    }
    let out = read_cef_str(s);
    sys::cef_string_userfree_utf16_free(s);
    out
}

/// Intrusive atomic refcounted container whose first field `c` starts with `cef_base_ref_counted_t`.
#[repr(C)]
pub struct CefObject<C, S> {
    pub c: C,
    rc: AtomicUsize,
    pub state: S,
    _pin: PhantomData<()>,
}

/// Trait identifying a CEF C struct whose initial field is `base: cef_base_ref_counted_t`.
///
/// # Safety
/// `C` MUST have `base: sys::cef_base_ref_counted_t` at offset 0.
pub unsafe trait CefStruct: Sized {
    fn base_mut(&mut self) -> &mut sys::cef_base_ref_counted_t;
}

macro_rules! impl_cef_struct {
    ($($ty:ty),* $(,)?) => {
        $(
            unsafe impl CefStruct for $ty {
                fn base_mut(&mut self) -> &mut sys::cef_base_ref_counted_t {
                    debug_assert_eq!(offset_of!($ty, base), 0);
                    &mut self.base
                }
            }
        )*
    };
}

impl_cef_struct!(
    sys::cef_app_t,
    sys::cef_browser_process_handler_t,
    sys::cef_client_t,
    sys::cef_render_handler_t,
    sys::cef_life_span_handler_t,
    sys::cef_display_handler_t,
    sys::cef_load_handler_t,
    sys::cef_request_handler_t,
    sys::cef_resource_request_handler_t,
    sys::cef_scheme_handler_factory_t,
    sys::cef_resource_handler_t,
    sys::cef_audio_handler_t,
    sys::cef_find_handler_t,
);

impl<C: CefStruct + Default, S> CefObject<C, S> {
    /// Allocates a new refcounted CEF callback object with reference count = 1.
    pub fn new(init_c: impl FnOnce(&mut C), state: S) -> *mut C {
        let mut c = C::default();
        init_c(&mut c);
        {
            let base = c.base_mut();
            base.size = size_of::<C>();
            base.add_ref = Some(Self::add_ref);
            base.release = Some(Self::release);
            base.has_one_ref = Some(Self::has_one_ref);
            base.has_at_least_one_ref = Some(Self::has_at_least_one_ref);
        }
        let boxed = Box::new(Self {
            c,
            rc: AtomicUsize::new(1),
            state,
            _pin: PhantomData,
        });
        Box::into_raw(boxed).cast::<C>()
    }

    /// Recovers `&S` from the raw `*mut C` callback pointer.
    ///
    /// # Safety
    /// `ptr` must have been created by `CefObject::<C, S>::new` and still be alive.
    pub unsafe fn state_from(ptr: *mut C) -> &'static S {
        let obj = &*(ptr.cast::<Self>());
        &obj.state
    }

    /// Increments the reference count and returns `ptr` for returning to CEF callers.
    ///
    /// # Safety
    /// `ptr` must be a valid live `CefObject<C, S>`.
    pub unsafe fn clone_ptr(ptr: *mut C) -> *mut C {
        if let Some(base) = ptr.cast::<sys::cef_base_ref_counted_t>().as_mut() {
            if let Some(f) = base.add_ref {
                f(base);
            }
        }
        ptr
    }

    unsafe extern "C" fn add_ref(base: *mut sys::cef_base_ref_counted_t) {
        let obj = &*(base.cast::<Self>());
        obj.rc.fetch_add(1, Ordering::Relaxed);
    }

    unsafe extern "C" fn release(base: *mut sys::cef_base_ref_counted_t) -> c_int {
        let ptr = base.cast::<Self>();
        let prev = (*ptr).rc.fetch_sub(1, Ordering::Release);
        if prev == 1 {
            std::sync::atomic::fence(Ordering::Acquire);
            drop(Box::from_raw(ptr));
            1
        } else {
            0
        }
    }

    unsafe extern "C" fn has_one_ref(base: *mut sys::cef_base_ref_counted_t) -> c_int {
        let obj = &*(base.cast::<Self>());
        i32::from(obj.rc.load(Ordering::Acquire) == 1)
    }

    unsafe extern "C" fn has_at_least_one_ref(base: *mut sys::cef_base_ref_counted_t) -> c_int {
        let obj = &*(base.cast::<Self>());
        i32::from(obj.rc.load(Ordering::Acquire) > 0)
    }
}

/// Decrements reference count on any CEF struct (`Release`).
///
/// # Safety
/// `ptr` must be null or a valid CEF refcounted object.
pub unsafe fn cef_release<T>(ptr: *mut T) {
    if !ptr.is_null() {
        let base = ptr.cast::<sys::cef_base_ref_counted_t>();
        if let Some(f) = (*base).release {
            f(base);
        }
    }
}

/// Increments reference count on any CEF struct (`AddRef`).
///
/// # Safety
/// `ptr` must be null or a valid CEF refcounted object.
pub unsafe fn cef_add_ref<T>(ptr: *mut T) {
    if !ptr.is_null() {
        let base = ptr.cast::<sys::cef_base_ref_counted_t>();
        if let Some(f) = (*base).add_ref {
            f(base);
        }
    }
}
