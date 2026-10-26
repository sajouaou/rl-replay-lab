// Prints when a player's jump, double jump and dodge go on and off (and his height), to check the replay marks them.
use subtr_actor::{PlayerFrame, ReplayDataCollector};

fn main() {
    let path = std::env::args().nth(1).expect("replay path");
    let who = std::env::args().nth(2).unwrap_or_else(|| "Anxoide".into());
    let data = std::fs::read(&path).expect("read");
    let replay = boxcars::ParserBuilder::new(&data).must_parse_network_data().parse().expect("parse");
    let d = ReplayDataCollector::new().get_replay_data(&replay).expect("data");
    for (_, pd) in &d.frame_data.players {
        let mut last = (false, false, false);
        let mut changes = 0;
        for (i, f) in pd.frames().iter().enumerate() {
            if let PlayerFrame::Data { rigid_body, jump_active, double_jump_active, dodge_active, player_name, .. } = f {
                if player_name.as_deref() != Some(who.as_str()) { break; }
                let now = (*jump_active, *double_jump_active, *dodge_active);
                if now != last && changes < 40 {
                    println!("{i:5} t={:.2} z={:.0} saut={} double={} flip={}", d.frame_data.metadata_frames[i].time,
                             rigid_body.location.z, now.0 as u8, now.1 as u8, now.2 as u8);
                    changes += 1;
                }
                last = now;
            }
        }
    }
}
