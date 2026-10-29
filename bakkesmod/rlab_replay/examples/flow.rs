// Prints how a replay plays its match, to see what the plugin must reproduce: the game state and kickoff countdown, the
// clock, goals (scorer, explosion), demolitions, which cars and ball are there, and the holes in time (where the game
// showed a goal again), frame by frame where anything changes.
//   cargo run --release --example flow -- <replay> [first frame] [last frame]
//   FLOW_CARS=1      every frame, with each car's place and speed
//   FLOW_SUMMARY=1   one line: the states it goes through, its holes, goals, demolitions, a score check
use std::collections::HashMap;

use boxcars::{Attribute, Replay};
use subtr_actor::{BallFrame, PlayerFrame, PlayerStatEventKind, ReplayData, ReplayDataCollector};

fn speed(v: Option<boxcars::Vector3f>) -> f32 { v.map_or(0.0, |v| (v.x * v.x + v.y * v.y + v.z * v.z).sqrt()) }

// the game state's name at a frame (an index in the replay's names: another number in another replay)
fn state_name<'a>(replay: &'a Replay, index: i32) -> &'a str { replay.names.get(index as usize).map(String::as_str).unwrap_or("?") }

fn summary(path: &str, replay: &Replay, d: &ReplayData) {
    let fd = &d.frame_data;
    let mut states: Vec<&str> = Vec::new();
    for m in &fd.metadata_frames {
        let s = state_name(replay, m.replicated_game_state_name);
        if states.last() != Some(&s) { states.push(s); }
    }
    let holes: Vec<String> = fd.metadata_frames.windows(2).filter(|w| w[1].time - w[0].time > 0.25)
        .map(|w| format!("{:.1} {}>{}", w[1].time - w[0].time, state_name(replay, w[0].replicated_game_state_name),
                         state_name(replay, w[1].replicated_game_state_name))).collect();
    // the score the replay records at each goal against the goals counted (a match joined late does not start at 0-0)
    let mut counted = (0, 0);
    let mut score_check = String::from("score ok");
    for g in &d.goal_events {
        if g.scoring_team_is_team_0 { counted.0 += 1 } else { counted.1 += 1 }
        if (g.team_zero_score, g.team_one_score) != (Some(counted.0), Some(counted.1)) {
            score_check = format!("SCORE {:?}-{:?} pour {}-{} comptes", g.team_zero_score, g.team_one_score, counted.0, counted.1);
            break;
        }
    }
    // each demolition: how many frames the car stays asleep where it blew up, then how many it is missing before it is back
    let demos: Vec<String> = d.demolish_infos.iter().filter_map(|dm| {
        let (_, pd) = fd.players.iter().find(|(p, _)| *p == dm.victim)?;
        let asleep = |i: usize| matches!(pd.frames().get(i), Some(PlayerFrame::Data { rigid_body, .. }) if rigid_body.sleeping);
        let run = (dm.frame..).take_while(|&i| asleep(i)).count();
        let gone = (dm.frame + run..fd.metadata_frames.len()).take_while(|&i| matches!(pd.frames().get(i), Some(PlayerFrame::Empty))).count();
        Some(format!("{run}+{gone}"))
    }).collect();
    // each assist: how many frames after its goal it is counted
    let assists: Vec<String> = d.player_stat_events.iter().filter(|e| matches!(e.kind, PlayerStatEventKind::Assist))
        .filter_map(|e| d.goal_events.iter().map(|g| e.frame as i64 - g.frame as i64).min_by_key(|x| x.abs()))
        .map(|x| format!("{x:+}")).collect();
    let mut kinds = states.clone();
    kinds.sort();
    kinds.dedup();
    let name = std::path::Path::new(path).file_stem().unwrap_or_default().to_string_lossy();
    println!("{name} etats {kinds:?} ({} changements) trous {holes:?} buts {} passes {assists:?} demos {demos:?} {score_check} fin {} s {}",
             states.len(), d.goal_events.len(), fd.metadata_frames.last().map_or(0, |m| m.seconds_remaining),
             states.last().copied().unwrap_or("?"));
}

// the attributes subtr-actor does not keep, by network frame (the same frames as subtr-actor's): the game event's state,
// countdown and score, the ball's explosion, actors made and deleted (the cars and ball of each kickoff are new ones)
fn raw_events(replay: &Replay) -> HashMap<usize, Vec<String>> {
    let object = |id: boxcars::ObjectId| replay.objects.get(usize::from(id)).map(String::as_str).unwrap_or("");
    let mut kinds: HashMap<boxcars::ActorId, String> = HashMap::new();
    let mut events = HashMap::new();
    let Some(net) = replay.network_frames.as_ref() else { return events };
    for (i, f) in net.frames.iter().enumerate() {
        let mut out = Vec::new();
        for na in &f.new_actors {
            let o = object(na.object_id);
            kinds.insert(na.actor_id, o.to_string());
            if o.contains("Ball") || o.starts_with("Archetypes.Car.") { out.push(format!("+{} {o}", na.actor_id.0)); }
        }
        for id in &f.deleted_actors {
            let o = kinds.get(id).cloned().unwrap_or_default();
            if o.contains("Ball") || o.starts_with("Archetypes.Car.") { out.push(format!("-{} {o}", id.0)); }
        }
        for ua in &f.updated_actors {
            let o = object(ua.object_id);
            const KEPT: &[&str] = &["ReplicatedRoundCountDownNumber", "ReplicatedStateName", "ReplicatedGameStateTimeRemaining",
                                    "bBallHasBeenHit", "ReplicatedScoredOnTeam", "Team_Soccar_TA:GameScore", "bOverTime", "RoundNum",
                                    "Actor:bHidden", "bMatchEnded", "HitTeamNum", "bIgnoreSyncing", "ReplicatedStateIndex"];
            if !o.contains("ExplosionData") && !o.contains("Demolish") && !KEPT.iter().any(|k| o.ends_with(k)) { continue; }
            let value = match &ua.attribute {
                Attribute::Int(v) if o.ends_with("ReplicatedStateName") => format!("{v} ({})", state_name(replay, *v)),
                Attribute::Int(v) => format!("{v}"),
                Attribute::Boolean(v) => format!("{v}"),
                Attribute::Byte(v) => format!("{v}"),
                other => format!("{other:?}").chars().take(200).collect(),
            };
            out.push(format!("{}[{}]={value}", o.rsplit(':').next().unwrap_or(o), ua.actor_id.0));
        }
        if !out.is_empty() { events.insert(i, out); }
    }
    events
}

fn main() {
    let path = std::env::args().nth(1).expect("replay path");
    let from: usize = std::env::args().nth(2).map_or(0, |a| a.parse().expect("first frame"));
    let to: usize = std::env::args().nth(3).map_or(usize::MAX, |a| a.parse().expect("last frame"));
    let data = std::fs::read(&path).expect("read");
    let replay = boxcars::ParserBuilder::new(&data).must_parse_network_data().parse().expect("parse");
    let d = ReplayDataCollector::new().get_replay_data(&replay).expect("data");
    if std::env::var("FLOW_SUMMARY").is_ok() {
        summary(&path, &replay, &d);
        return;
    }
    let fd = &d.frame_data;
    let who = |id: &subtr_actor::PlayerId| fd.players.iter().position(|(p, _)| p == id).map_or(-1, |k| k as i32);
    let names: Vec<String> = fd.players.iter().map(|(_, pd)| pd.frames().iter().find_map(|f| match f {
        PlayerFrame::Data { player_name, .. } => player_name.clone(),
        _ => None,
    }).unwrap_or_default()).collect();
    println!("joueurs {names:?}");
    for g in &d.goal_events {
        println!("but image {} t={:.2} bleus={} buteur {} score {:?}-{:?}", g.frame, g.time, g.scoring_team_is_team_0,
                 g.player.as_ref().map_or(-1, who), g.team_zero_score, g.team_one_score);
    }
    for dm in &d.demolish_infos {
        println!("demo image {} t={:.2} victime {} par {}", dm.frame, dm.time, who(&dm.victim), who(&dm.attacker));
    }

    let events = raw_events(&replay);
    let every_frame = std::env::var("FLOW_CARS").is_ok();
    let mut last = String::new();
    for i in from..fd.metadata_frames.len().min(to) {
        let m = &fd.metadata_frames[i];
        let ball = match fd.ball_data.frames().get(i) {
            Some(BallFrame::Data { rigid_body: r }) => format!("ballon ({:.0},{:.0},{:.0}) v{:.0}{}", r.location.x, r.location.y,
                                                              r.location.z, speed(r.linear_velocity), if r.sleeping { " dort" } else { "" }),
            _ => "ballon -".into(),
        };
        let state = state_name(&replay, m.replicated_game_state_name);
        if every_frame {
            let cars: Vec<String> = fd.players.iter().map(|(_, pd)| match pd.frames().get(i) {
                Some(PlayerFrame::Data { rigid_body: r, .. }) => format!("({:.0},{:.0},{:.0}) v{:.0}{}", r.location.x, r.location.y,
                                                                        r.location.z, speed(r.linear_velocity), if r.sleeping { "z" } else { "" }),
                _ => "-".into(),
            }).collect();
            println!("{i:5} t={:7.2} {state} {ball} | {}", m.time, cars.join(" "));
            continue;
        }
        if i > 0 && m.time - fd.metadata_frames[i - 1].time > 0.25 {
            println!("      ... {:.2} s sans image", m.time - fd.metadata_frames[i - 1].time);
        }
        // X: a car there, z: there but asleep (demolished), .: missing
        let cars: String = fd.players.iter().map(|(_, pd)| match pd.frames().get(i) {
            Some(PlayerFrame::Data { rigid_body, .. }) => if rigid_body.sleeping { 'z' } else { 'X' },
            _ => '.',
        }).collect();
        let line = format!("sec {} etat {state} voitures {cars}", m.seconds_remaining);
        let ev = events.get(&i);
        if line != last || ev.is_some() {
            println!("{i:5} t={:7.2} {line} {ball}{}", m.time, ev.map_or(String::new(), |e| format!("   {}", e.join("  "))));
            last = line;
        }
    }
}
