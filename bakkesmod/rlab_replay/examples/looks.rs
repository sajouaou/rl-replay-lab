fn main() {
    for path in std::env::args().skip(1) {
        match rlab_replay::looks_text(&path) {
            Ok(s) => print!("{s}"),
            Err(e) => println!("{path}: ERREUR {e}"),
        }
    }
}
