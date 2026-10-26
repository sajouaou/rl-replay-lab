// Prints each player's camera as the blob will carry it, and checks the time base against subtr-actor's frames.
fn main() {
    let path = std::env::args().nth(1).expect("replay path");
    let data = std::fs::read(&path).expect("read");
    let replay = boxcars::ParserBuilder::new(&data).must_parse_network_data().parse().expect("parse");
    let d = subtr_actor::ReplayDataCollector::new().get_replay_data(&replay).expect("data");
    let net = replay.network_frames.as_ref().unwrap();
    println!("frames: network {} subtr-actor {}; times {} / {}, last {} / {}", net.frames.len(), d.frame_data.metadata_frames.len(),
             net.frames[0].time, d.frame_data.metadata_frames[0].time, net.frames.last().unwrap().time,
             d.frame_data.metadata_frames.last().unwrap().time);
    println!("ball: {}", rlab_replay::ball_archetype(&replay));
    let at: usize = std::env::args().nth(2).map_or(600, |a| a.parse().unwrap());
    let t = net.frames[at].time;
    println!("frame {at} t={t}");
    for (name, cam) in rlab_replay::cameras(&replay) {
        let mut on = cam.ball_cam.first().map_or(false, |c| c.1);
        for &(ct, v) in &cam.ball_cam { if ct > t { break; } on = v; }
        println!("  {name}: ball cam {on} at frame {at}");
        if !cam.ball_cam.is_empty() {
            println!("    changes 39-60 s: {:?}", cam.ball_cam.iter().filter(|c| c.0 > 39.0 && c.0 < 60.0).collect::<Vec<_>>());
        }
        println!("{name}: {:?}, {} ball cam changes, first {:?}", cam.settings, cam.ball_cam.len(), &cam.ball_cam[..cam.ball_cam.len().min(3)]);
    }
}
