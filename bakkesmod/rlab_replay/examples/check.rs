fn main() {
    for path in std::env::args().skip(1) {
        let t = std::time::Instant::now();
        match rlab_replay::summary(&path) {
            Ok(s) => println!("{path}: {:.2}s\n{s}", t.elapsed().as_secs_f32()),
            Err(e) => println!("{path}: ERREUR {e}"),
        }
    }
}
