//! Reads Rocket League `.replay` files for the Replay Lab BakkesMod plugin.
//!
//! Exposed as a C ABI so the plugin (C++, loaded inside the game) can call it:
//! - `rlab_parse_replay`: every frame of the ball and the cars, goals and saves, as one binary blob
//! - `rlab_replay_header`: just the header (map, date, score, players) as text, for the replay list
//! - `rlab_free`: release a blob returned by either function
//!
//! Blob layout (little endian), version 1:
//! ```text
//! "RLAB" u32 version
//! str map, str date                         (str = u16 length + UTF-8 bytes)
//! u32 frames, u32 players
//! players: str name, u8 team
//! frames:  f32 time, i32 seconds_remaining,
//!          ball: u8 valid, f32 x3 position, f32 x4 rotation (x, y, z, w), f32 x3 velocity, f32 x3 angular velocity (rad/s)
//!          each player: same rigid body + u8 boost (0-255) + u8 boosting
//! u32 goals:  u32 frame, u8 scoring team
//! u32 saves:  u32 frame, u8 player index
//! version 2, each player's car as it looks in the replay:
//!          u8 known; if known: u8 slots, each (u8 slot, u32 product, i32 paint or -1),
//!          u8 team colour, u8 accent colour, u32 team finish, u32 accent finish
//! version 3: str ball archetype (the game mode: "Archetypes.Ball.Ball_BasketBall" in Hoops...),
//!          then each player's camera: u8 known; if known: f32 fov, height, angle, distance, stiffness, swivel, transition,
//!          u32 ball cam changes, each (f32 time, u8 on)
//! version 4: each player's frame also has u8 moves after boosting: 1 jumping, 2 double jumping, 4 dodging
//! ```
use std::collections::HashMap;
use std::ffi::{c_char, CStr};
use std::panic;

use boxcars::{ActorId, Attribute, HeaderProp, ParserBuilder, ProductValue, RigidBody};
use subtr_actor::{BallFrame, PlayerFrame, PlayerStatEventKind, ReplayDataCollector};

const VERSION: u32 = 4;

struct Blob(Vec<u8>);

impl Blob {
    fn u8(&mut self, v: u8) { self.0.push(v); }
    fn u32(&mut self, v: u32) { self.0.extend_from_slice(&v.to_le_bytes()); }
    fn i32(&mut self, v: i32) { self.0.extend_from_slice(&v.to_le_bytes()); }
    fn f32(&mut self, v: f32) { self.0.extend_from_slice(&v.to_le_bytes()); }
    fn str(&mut self, s: &str) {
        let b = &s.as_bytes()[..s.len().min(u16::MAX as usize)];
        self.0.extend_from_slice(&(b.len() as u16).to_le_bytes());
        self.0.extend_from_slice(b);
    }
    fn body(&mut self, rb: Option<&RigidBody>) {
        match rb {
            None => {
                self.u8(0);
                for _ in 0..13 { self.f32(0.0); }
            }
            Some(rb) => {
                self.u8(1);
                for v in [rb.location.x, rb.location.y, rb.location.z] { self.f32(v); }
                for v in [rb.rotation.x, rb.rotation.y, rb.rotation.z, rb.rotation.w] { self.f32(v); }
                let lv = rb.linear_velocity.unwrap_or(boxcars::Vector3f { x: 0.0, y: 0.0, z: 0.0 });
                for v in [lv.x, lv.y, lv.z] { self.f32(v); }
                // replays store angular velocity in rad/s * 100
                let av = rb.angular_velocity.unwrap_or(boxcars::Vector3f { x: 0.0, y: 0.0, z: 0.0 });
                for v in [av.x, av.y, av.z] { self.f32(v / 100.0); }
            }
        }
    }
}

/// What a player's car looks like: the product in each loadout slot with its paint, and the team colours.
#[derive(Debug, Default, Clone)]
pub struct Look {
    pub slots: Vec<(u8, u32, i32)>,
    pub team_color: u8,
    pub accent_color: u8,
    pub team_finish: u32,
    pub accent_finish: u32,
}

/// Each player's look (by name), from the PRI loadouts and the car's team paint in the network data.
pub fn looks(replay: &boxcars::Replay) -> HashMap<String, Look> {
    let Some(frames) = replay.network_frames.as_ref() else { return HashMap::new() };
    let name_of = |id: boxcars::ObjectId| replay.objects.get(id.0 as usize).map(String::as_str).unwrap_or("");
    let mut objects: HashMap<ActorId, &str> = HashMap::new();
    let mut names: HashMap<ActorId, String> = HashMap::new();
    let mut teams: HashMap<ActorId, ActorId> = HashMap::new();
    let mut loadouts: HashMap<ActorId, boxcars::TeamLoadout> = HashMap::new();
    let mut online: HashMap<ActorId, boxcars::LoadoutsOnline> = HashMap::new();
    let mut car_pri: HashMap<ActorId, ActorId> = HashMap::new();
    let mut paints: HashMap<ActorId, boxcars::TeamPaint> = HashMap::new();   // by PRI
    for frame in &frames.frames {
        for na in &frame.new_actors {
            objects.insert(na.actor_id, name_of(na.object_id));
        }
        for ua in &frame.updated_actors {
            match (name_of(ua.object_id), &ua.attribute) {
                ("Engine.PlayerReplicationInfo:PlayerName", Attribute::String(s)) => { names.insert(ua.actor_id, s.clone()); }
                ("Engine.PlayerReplicationInfo:Team", Attribute::ActiveActor(a)) => { teams.insert(ua.actor_id, a.actor); }
                ("TAGame.PRI_TA:ClientLoadouts", Attribute::TeamLoadout(l)) => { loadouts.entry(ua.actor_id).or_insert(**l); }
                ("TAGame.PRI_TA:ClientLoadoutsOnline", Attribute::LoadoutsOnline(l)) => { online.entry(ua.actor_id).or_insert(l.clone()); }
                ("Engine.Pawn:PlayerReplicationInfo", Attribute::ActiveActor(a)) => { car_pri.insert(ua.actor_id, a.actor); }
                ("TAGame.Car_TA:TeamPaint", Attribute::TeamPaint(p)) => {
                    if let Some(pri) = car_pri.get(&ua.actor_id) { paints.entry(*pri).or_insert(*p); }
                }
                _ => {}
            }
        }
    }
    let mut out = HashMap::new();
    for (pri, name) in &names {
        let Some(l) = loadouts.get(pri) else { continue };
        let orange = teams.get(pri).and_then(|t| objects.get(t)).map_or(false, |o| o.ends_with("Team1"));
        let lo = if orange { &l.orange } else { &l.blue };
        let products = [(0u8, Some(lo.body)), (1, Some(lo.decal)), (2, Some(lo.wheels)), (3, Some(lo.rocket_trail)),
                        (4, Some(lo.antenna)), (5, Some(lo.topper)), (13, lo.engine_audio), (14, lo.trail),
                        (15, lo.goal_explosion), (16, lo.banner)];
        let paint_of = |slot: u8| -> i32 {
            let Some(on) = online.get(pri) else { return -1 };
            let side = if orange { &on.orange } else { &on.blue };
            side.get(slot as usize).and_then(|attrs| attrs.iter().find_map(|a| match a.value {
                ProductValue::OldPaint(p) | ProductValue::NewPaint(p) => Some(p as i32),
                _ => None,
            })).unwrap_or(-1)
        };
        let mut look = Look::default();
        for (slot, product) in products {
            if let Some(id) = product.filter(|&id| id != 0) { look.slots.push((slot, id, paint_of(slot))); }
        }
        if let Some(p) = paints.get(pri) {
            look.team_color = p.primary_color;
            look.accent_color = p.accent_color;
            look.team_finish = p.primary_finish;
            look.accent_finish = p.accent_finish;
        }
        out.insert(name.clone(), look);
    }
    out
}

/// A player's camera as the replay recorded it: settings, and when ball cam went on and off.
#[derive(Debug, Default, Clone)]
pub struct Camera {
    pub settings: Option<[f32; 7]>,   // fov, height, angle, distance, stiffness, swivel, transition
    pub ball_cam: Vec<(f32, bool)>,
}

/// Each player's camera (by name), from the camera settings actors of the network data.
pub fn cameras(replay: &boxcars::Replay) -> HashMap<String, Camera> {
    let Some(frames) = replay.network_frames.as_ref() else { return HashMap::new() };
    let name_of = |id: boxcars::ObjectId| replay.objects.get(id.0 as usize).map(String::as_str).unwrap_or("");
    let mut names: HashMap<ActorId, String> = HashMap::new();
    let mut cam_pri: HashMap<ActorId, ActorId> = HashMap::new();
    let mut by_pri: HashMap<ActorId, Camera> = HashMap::new();
    for frame in &frames.frames {
        for id in &frame.deleted_actors { cam_pri.remove(id); }
        // who each camera belongs to first: a frame may list its values before its player
        for ua in &frame.updated_actors {
            match (name_of(ua.object_id), &ua.attribute) {
                ("Engine.PlayerReplicationInfo:PlayerName", Attribute::String(s)) => { names.insert(ua.actor_id, s.clone()); }
                ("TAGame.CameraSettingsActor_TA:PRI", Attribute::ActiveActor(a)) => { cam_pri.insert(ua.actor_id, a.actor); }
                _ => {}
            }
        }
        for ua in &frame.updated_actors {
            let Some(pri) = cam_pri.get(&ua.actor_id) else { continue };
            let cam = by_pri.entry(*pri).or_default();
            match (name_of(ua.object_id), &ua.attribute) {
                ("TAGame.CameraSettingsActor_TA:ProfileSettings", Attribute::CamSettings(c)) => {
                    cam.settings = Some([c.fov, c.height, c.angle, c.distance, c.stiffness, c.swivel, c.transition.unwrap_or(1.0)]);
                }
                ("TAGame.CameraSettingsActor_TA:bUsingSecondaryCamera", Attribute::Boolean(on)) => {
                    if cam.ball_cam.last().map_or(true, |&(_, was)| was != *on) { cam.ball_cam.push((frame.time, *on)); }
                }
                _ => {}
            }
        }
    }
    by_pri.into_iter().filter_map(|(pri, cam)| names.get(&pri).map(|n| (n.clone(), cam))).collect()
}

/// The ball the replay's game used: it tells the game mode.
pub fn ball_archetype(replay: &boxcars::Replay) -> String {
    let Some(frames) = replay.network_frames.as_ref() else { return String::new() };
    frames.frames.iter().flat_map(|f| f.new_actors.iter())
        .filter_map(|na| replay.objects.get(na.object_id.0 as usize))
        .find(|name| name.starts_with("Archetypes.Ball."))
        .cloned().unwrap_or_default()
}

/// The looks as text, for checking a replay by hand.
pub fn looks_text(path: &str) -> Result<String, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("lecture de {path}: {e}"))?;
    let replay = ParserBuilder::new(&bytes).must_parse_network_data().on_error_check_crc().parse()
        .map_err(|e| format!("replay illisible: {e}"))?;
    let mut out = String::new();
    for (name, look) in looks(&replay) {
        out += &format!("{name}: {:?}\n", look);
    }
    Ok(out)
}

fn header_str(replay: &boxcars::Replay, key: &str) -> String {
    replay.properties.iter().find(|(k, _)| k == key).map(|(_, v)| match v {
        HeaderProp::Str(s) | HeaderProp::Name(s) => s.clone(),
        HeaderProp::Int(i) => i.to_string(),
        _ => String::new(),
    }).unwrap_or_default()
}

fn parse(path: &str) -> Result<Vec<u8>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("lecture de {path}: {e}"))?;
    let replay = ParserBuilder::new(&bytes).must_parse_network_data().on_error_check_crc().parse()
        .map_err(|e| format!("replay illisible: {e}"))?;
    let data = ReplayDataCollector::new().get_replay_data(&replay).map_err(|e| format!("{e:?}"))?;
    let fd = &data.frame_data;
    let n = fd.metadata_frames.len();

    // keep players that appear at least once, with their name and team
    let mut players = Vec::new();
    for (id, pd) in &fd.players {
        let mut name = None;
        let mut team = None;
        for f in pd.frames() {
            if let PlayerFrame::Data { player_name, is_team_0, .. } = f {
                if name.is_none() { name = player_name.clone(); }
                if team.is_none() { team = is_team_0.map(|b| if b { 0u8 } else { 1u8 }); }
            }
        }
        if pd.frames().iter().any(|f| matches!(f, PlayerFrame::Data { .. })) {
            players.push((id.clone(), name.unwrap_or_else(|| "?".into()), team.unwrap_or(0), pd));
        }
    }

    let mut b = Blob(Vec::with_capacity(64 + n * (60 + players.len() * 64)));
    b.0.extend_from_slice(b"RLAB");
    b.u32(VERSION);
    b.str(&header_str(&replay, "MapName"));
    b.str(&header_str(&replay, "Date"));
    b.u32(n as u32);
    b.u32(players.len() as u32);
    for (_, name, team, _) in &players {
        b.str(name);
        b.u8(*team);
    }
    let ball_frames = fd.ball_data.frames();
    for i in 0..n {
        b.f32(fd.metadata_frames[i].time);
        b.i32(fd.metadata_frames[i].seconds_remaining);
        match ball_frames.get(i) {
            Some(BallFrame::Data { rigid_body }) => b.body(Some(rigid_body)),
            _ => b.body(None),
        }
        for (_, _, _, pd) in &players {
            match pd.frames().get(i) {
                Some(PlayerFrame::Data { rigid_body, boost_amount, boost_active, jump_active, double_jump_active, dodge_active, .. }) => {
                    b.body(Some(rigid_body));
                    b.u8(boost_amount.clamp(0.0, 255.0) as u8);
                    b.u8(*boost_active as u8);
                    b.u8(*jump_active as u8 | (*double_jump_active as u8) << 1 | (*dodge_active as u8) << 2);
                }
                _ => {
                    b.body(None);
                    b.u8(0);
                    b.u8(0);
                    b.u8(0);
                }
            }
        }
    }
    b.u32(data.goal_events.len() as u32);
    for g in &data.goal_events {
        b.u32(g.frame as u32);
        b.u8(if g.scoring_team_is_team_0 { 0 } else { 1 });
    }
    let saves: Vec<_> = data.player_stat_events.iter()
        .filter(|e| matches!(e.kind, PlayerStatEventKind::Save))
        .filter_map(|e| players.iter().position(|(id, ..)| *id == e.player).map(|k| (e.frame, k)))
        .collect();
    b.u32(saves.len() as u32);
    for (frame, k) in saves {
        b.u32(frame as u32);
        b.u8(k as u8);
    }
    let looks = looks(&replay);
    for (_, name, _, _) in &players {
        match looks.get(name) {
            None => b.u8(0),
            Some(look) => {
                b.u8(1);
                b.u8(look.slots.len() as u8);
                for &(slot, product, paint) in &look.slots {
                    b.u8(slot);
                    b.u32(product);
                    b.i32(paint);
                }
                b.u8(look.team_color);
                b.u8(look.accent_color);
                b.u32(look.team_finish);
                b.u32(look.accent_finish);
            }
        }
    }
    b.str(&ball_archetype(&replay));
    let cameras = cameras(&replay);
    for (_, name, _, _) in &players {
        match cameras.get(name) {
            Some(Camera { settings: Some(set), ball_cam }) => {
                b.u8(1);
                for v in set { b.f32(*v); }
                b.u32(ball_cam.len() as u32);
                for &(time, on) in ball_cam {
                    b.f32(time);
                    b.u8(on as u8);
                }
            }
            _ => b.u8(0),
        }
    }
    Ok(b.0)
}

fn header(path: &str) -> Result<Vec<u8>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("lecture de {path}: {e}"))?;
    let replay = ParserBuilder::new(&bytes).never_parse_network_data().on_error_check_crc().parse()
        .map_err(|e| format!("replay illisible: {e}"))?;
    let mut out = String::new();
    for key in ["MapName", "Date", "TeamSize", "Team0Score", "Team1Score"] {
        out += &format!("{key}={}\n", header_str(&replay, key));
    }
    if let Some((_, HeaderProp::Array(stats))) = replay.properties.iter().find(|(k, _)| k == "PlayerStats") {
        for player in stats {
            let get = |key: &str| player.iter().find(|(k, _)| k == key).map(|(_, v)| match v {
                HeaderProp::Str(s) | HeaderProp::Name(s) => s.clone(),
                HeaderProp::Int(i) => i.to_string(),
                _ => String::new(),
            }).unwrap_or_default();
            out += &format!("Player={};{}\n", get("Name"), get("Team"));
        }
    }
    Ok(out.into_bytes())
}

/// Hands a byte buffer to C. Returns 0 on success; on failure writes a message into `err`.
unsafe fn export(result: std::thread::Result<Result<Vec<u8>, String>>, out_ptr: *mut *mut u8, out_len: *mut usize,
                 err: *mut c_char, err_len: usize) -> i32 {
    let msg = match result {
        Ok(Ok(v)) => {
            let boxed = v.into_boxed_slice();
            *out_len = boxed.len();
            *out_ptr = Box::into_raw(boxed) as *mut u8;
            return 0;
        }
        Ok(Err(e)) => e,
        Err(_) => "erreur interne en lisant le replay".to_string(),
    };
    if !err.is_null() && err_len > 0 {
        let b = msg.as_bytes();
        let n = b.len().min(err_len - 1);
        std::ptr::copy_nonoverlapping(b.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
    1
}

unsafe fn path_arg(path: *const c_char) -> String {
    CStr::from_ptr(path).to_string_lossy().into_owned()
}

/// # Safety
/// `path` must be a NUL-terminated UTF-8 path; `out_ptr`/`out_len` must be valid; free the blob with `rlab_free`.
#[no_mangle]
pub unsafe extern "C" fn rlab_parse_replay(path: *const c_char, out_ptr: *mut *mut u8, out_len: *mut usize,
                                           err: *mut c_char, err_len: usize) -> i32 {
    let p = path_arg(path);
    export(panic::catch_unwind(|| parse(&p)), out_ptr, out_len, err, err_len)
}

/// # Safety
/// Same contract as `rlab_parse_replay`; the blob is UTF-8 text, one `key=value` per line.
#[no_mangle]
pub unsafe extern "C" fn rlab_replay_header(path: *const c_char, out_ptr: *mut *mut u8, out_len: *mut usize,
                                            err: *mut c_char, err_len: usize) -> i32 {
    let p = path_arg(path);
    export(panic::catch_unwind(|| header(&p)), out_ptr, out_len, err, err_len)
}

/// # Safety
/// `ptr`/`len` must come from one of the functions above, and be freed only once.
#[no_mangle]
pub unsafe extern "C" fn rlab_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
    }
}

/// Native check used by the example: parse a replay and summarize the blob.
pub fn summary(path: &str) -> Result<String, String> {
    let blob = parse(path)?;
    let head = String::from_utf8(header(path)?).unwrap_or_default();
    Ok(format!("{} octets\n{}", blob.len(), head))
}

/// The raw blob, for native tools and tests.
pub fn parse_to_blob(path: &str) -> Result<Vec<u8>, String> {
    parse(path)
}
