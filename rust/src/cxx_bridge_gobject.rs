use cxx::{type_id, ExternType};

pub trait FFIGObjectWrapper {
    type Wrapper;

    fn gobj_wrap(&mut self) -> Self::Wrapper;
}

macro_rules! wrap {
    ($w:ident, $bound:path) => {
        impl FFIGObjectWrapper for $w {
            type Wrapper = $bound;
            fn gobj_wrap(&mut self) -> Self::Wrapper {
                unsafe { glib::translate::from_glib_none(&mut self.0 as *mut _) }
            }
        }
    }
}

#[repr(transparent)]
pub struct FFIOstreeRepo(ostree_sys::OstreeRepo);

unsafe impl ExternType for FFIOstreeRepo {
    type Id = type_id!("rpmostreecxx::OstreeRepo");
    type Kind = cxx::kind::Trivial;
}
wrap!(FFIOstreeRepo, ostree::Repo);

#[repr(transparent)]
pub struct FFIOstreeDeployment(ostree_sys::OstreeDeployment);

unsafe impl ExternType for FFIOstreeDeployment {
    type Id = type_id!("rpmostreecxx::OstreeDeployment");
    type Kind = cxx::kind::Trivial;
}
wrap!(FFIOstreeDeployment, ostree::Deployment);
