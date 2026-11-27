//! Says whether the memory bridge loads here, through the crate's own loader:
//! `victorybridge.dll` beside this program (or on Wine's builtin path),
//! `VICTORY_BRIDGE` set and not `0`. Used by `bridge/pe/test/run.sh` to check
//! the Rust side end to end under Proton. Exit code 0 when the bridge is
//! available, or with the argument `off` when it is not.

fn main() {
    let want_on = !std::env::args().any(|a| a == "off");
    let on = victory_bridge::available();
    println!(
        "VICTORY_BRIDGE={:?}: asked {}, available {on}",
        std::env::var("VICTORY_BRIDGE").ok(),
        victory_bridge::asked()
    );
    std::process::exit(if on == want_on { 0 } else { 1 });
}
