// Prints where the ball was over the frames before each goal of a replay.
use subtr_actor::{BallFrame, ReplayDataCollector};

fn main() {
    let path = std::env::args().nth(1).expect("replay path");
    let data = std::fs::read(&path).expect("read");
    let replay = boxcars::ParserBuilder::new(&data).must_parse_network_data().parse().expect("parse");
    let d = ReplayDataCollector::new().get_replay_data(&replay).expect("data");
    let balls = d.frame_data.ball_data.frames();
    for g in &d.goal_events {
        println!("goal frame {} team0={}", g.frame, g.scoring_team_is_team_0);
        for i in g.frame.saturating_sub(12)..=g.frame + 2 {
            if let Some(BallFrame::Data { rigid_body }) = balls.get(i) {
                let l = rigid_body.location;
                let v = rigid_body.linear_velocity.unwrap_or(boxcars::Vector3f { x: 0., y: 0., z: 0. });
                println!("  {i} t={:.2} pos ({:.0},{:.0},{:.0}) vel ({:.0},{:.0},{:.0}) r_xy_from_hoop {:.0}", d.frame_data.metadata_frames[i].time,
                         l.x, l.y, l.z, v.x, v.y, v.z, (l.x * l.x + (l.y.abs() - 2838.0).powi(2)).sqrt());
            }
        }
    }
}
