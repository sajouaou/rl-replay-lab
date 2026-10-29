// Lists the saves made against a player's shots in each replay: when, who saved, who shot (the attacker nearest the
// ball 1 s before), with the map, the mode (ball) and the score, to pick a moment to take over.
use subtr_actor::{BallFrame, PlayerFrame, PlayerStatEventKind, ReplayDataCollector};

fn main() {
    let me = std::env::var("RL_PLAYER").unwrap_or_else(|_| "Anxoide".into());
    for path in std::env::args().skip(1) {
        let Ok(data) = std::fs::read(&path) else { continue };
        let Ok(replay) = boxcars::ParserBuilder::new(&data).must_parse_network_data().parse() else { continue };
        let ball = rlab_replay::ball_archetype(&replay);
        if !ball.ends_with("Ball_Default") { continue; }
        let Ok(d) = ReplayDataCollector::new().get_replay_data(&replay) else { continue };
        let fd = &d.frame_data;
        let mut players = Vec::new();   // (id, name, team0)
        for (id, pd) in &fd.players {
            let mut name = None;
            let mut team0 = None;
            for f in pd.frames() {
                if let PlayerFrame::Data { player_name, is_team_0, .. } = f {
                    name = name.or(player_name.clone());
                    team0 = team0.or(*is_team_0);
                }
            }
            players.push((id.clone(), name.unwrap_or_default(), team0.unwrap_or(true)));
        }
        let Some(mine) = players.iter().find(|p| p.1 == me) else { continue };
        let map = replay.properties.iter().find(|(k, _)| k == "MapName").map(|(_, v)| format!("{v:?}")).unwrap_or_default();
        let size = players.len();
        let name = std::path::Path::new(&path).file_stem().unwrap().to_string_lossy().to_string();
        for e in &d.player_stat_events {
            if !matches!(e.kind, PlayerStatEventKind::Save) { continue; }
            let Some(saver) = players.iter().find(|p| p.0 == e.player) else { continue };
            if saver.2 == mine.2 { continue; }   // a save against my team's shot
            let before = e.frame.saturating_sub(30);
            let ball_at = match fd.ball_data.frames().get(before) { Some(BallFrame::Data { rigid_body }) => Some(rigid_body.location), _ => None };
            let shooter = ball_at.and_then(|b| players.iter().filter(|p| p.2 == mine.2).filter_map(|p| {
                match fd.players.iter().find(|(id, _)| *id == p.0)?.1.frames().get(before) {
                    Some(PlayerFrame::Data { rigid_body, .. }) => {
                        let c = rigid_body.location;
                        Some((((c.x - b.x).powi(2) + (c.y - b.y).powi(2) + (c.z - b.z).powi(2)).sqrt(), p.1.clone()))
                    }
                    _ => None,
                }
            }).min_by(|a, b| a.0.total_cmp(&b.0)).map(|(_, n)| n));
            println!("{name} {map} {size}j frame {} t={:.1} arret de {} tir de {}", e.frame, fd.metadata_frames[e.frame].time,
                     saver.1, shooter.unwrap_or_default());
        }
    }
}
