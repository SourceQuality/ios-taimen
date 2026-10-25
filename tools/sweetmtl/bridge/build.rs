// SPDX-License-Identifier: MIT
//! The self-test's fixture libraries (NativeMetalBridge.rs's FIXTURE_AIR, NativeMetalObjects.rs's FUNCTION_CONSTANT_AIR):
//! metal2vulkan's own LLVM-text fixtures assembled to AIR bitcode with llvm-as (LLVM 21 or newer; LLVM_AS names it,
//! default `llvm-as` on PATH), as flame's tools/build-m2v-bridge.sh does. NMB_FIXTURE_AIR and NMB_FC_AIR, when set,
//! name ready-made AIR files instead.

use std::path::{Path, PathBuf};
use std::process::Command;

fn assemble(ll: &Path, out: &Path) {
    let llvm_as = std::env::var("LLVM_AS").unwrap_or_else(|_| "llvm-as".into());
    let status = Command::new(&llvm_as)
        .arg("-o")
        .arg(out)
        .arg(ll)
        .status()
        .unwrap_or_else(|e| panic!("cannot run {llvm_as} (set LLVM_AS to an LLVM 21+ llvm-as): {e}"));
    assert!(status.success(), "{llvm_as} {} failed", ll.display());
}

fn main() {
    let here = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let fixtures = here.join("../m2v/crates/metal2vulkan-validation/fixtures/public");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    println!("cargo:rerun-if-env-changed=NMB_FIXTURE_AIR");
    println!("cargo:rerun-if-env-changed=NMB_FC_AIR");
    println!("cargo:rerun-if-env-changed=LLVM_AS");
    for (var, ll, name) in [
        ("NMB_FIXTURE_AIR", fixtures.join("examples/runtime_examples.ll"), "runtime_examples.air"),
        ("NMB_FC_AIR", fixtures.join("kernel_function_constant_gate_fold.ll"), "function_constant.air"),
    ] {
        if std::env::var_os(var).is_some() {
            continue;
        }
        let air = out.join(name);
        println!("cargo:rerun-if-changed={}", ll.display());
        assemble(&ll, &air);
        println!("cargo:rustc-env={var}={}", air.display());
    }
}
