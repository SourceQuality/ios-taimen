// SPDX-License-Identifier: MIT
//! `Library` and `Function` (`MTLLibrary`, `MTLFunction`): the translator's library, parsed once;
//! functions are compiled when a pipeline is made of them.

use crate::device::Device;
use crate::error::{invalid, Error};
use metal2vulkan::library::FunctionConstantValues;
use metal2vulkan::metal::FunctionType;
use metal2vulkan::pipeline::FunctionDescriptor;
use metal2vulkan::Budget;

/// `MTLLibrary`.
#[derive(Clone)]
pub struct Library {
    pub(crate) device: Device,
    pub(crate) library: metal2vulkan::library::Library,
}

impl Device {
    /// A library of a translator library made by the caller (`Library::from_air`, `parse`).
    pub fn new_library_with_compile_library(&self, library: metal2vulkan::library::Library) -> Library {
        Library { device: self.clone(), library }
    }

    /// `newLibraryWithData:error:`: a metallib or AIR module.
    pub fn new_library_with_data(&self, bytes: &[u8]) -> Result<Library, Error> {
        Ok(Library { device: self.clone(), library: metal2vulkan::library::Library::parse(bytes)? })
    }
}

impl Library {
    /// `functionNames`, in the library's order.
    pub fn function_names(&self) -> Vec<String> {
        self.library.functions().map(|f| f.name.to_string()).collect()
    }

    /// `newFunctionWithName:`.
    pub fn new_function_with_name(&self, name: &str) -> Result<Function, Error> {
        Ok(Function {
            device: self.device.clone(),
            function: self.library.function(name)?,
            constants: FunctionConstantValues::defaults(),
        })
    }

    /// `newFunctionWithName:constantValues:error:`.
    pub fn new_function_with_name_constant_values(&self, name: &str, values: &FunctionConstantValues)
                                                  -> Result<Function, Error> {
        Ok(Function { device: self.device.clone(), function: self.library.function(name)?, constants: values.clone() })
    }

    pub fn device(&self) -> Device {
        self.device.clone()
    }
}

/// `MTLFunction`.
#[derive(Clone)]
pub struct Function {
    pub(crate) device: Device,
    pub(crate) function: metal2vulkan::library::Function,
    pub(crate) constants: FunctionConstantValues,
}

impl Function {
    /// The translator's descriptor of the function with its constant values.
    pub fn function_descriptor(&self) -> FunctionDescriptor {
        let mut d = FunctionDescriptor::new(self.function.clone());
        d.constants = self.constants.clone();
        d
    }

    /// `functionType`, as the translator states the function.
    pub fn function_type(&self) -> Result<FunctionType, Error> {
        let properties = self.function.properties(&Budget::standard())?;
        Ok(properties.function_type())
    }

    pub fn name(&self) -> String {
        self.function.info().name.to_string()
    }

    pub fn device(&self) -> Device {
        self.device.clone()
    }
}

#[allow(dead_code)]
fn _unused() -> Error {
    invalid("")
}
