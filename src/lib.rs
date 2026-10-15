//! `moba_engine` — a Dota-style MOBA simulation engine that compiles to WebAssembly.
//!
//! The crate is a plain deterministic simulation (`Game`) plus a thin C-ABI layer
//! (`ffi`) that exposes flat buffers to JavaScript. There are no dependencies, so
//! the same code runs natively (tests, headless matches) and in the browser.

pub mod abilities;
pub mod ai;
pub mod combat;
pub mod content;
pub mod data;
pub mod ffi;
pub mod game;
pub mod json;
pub mod map;
pub mod math;
pub mod path;
pub mod unit;

pub use game::{Config, Game};
