/*
 ESP32-P4 6.2" INTEGRAL FRONT-JAW MONITOR CLAMP -- V4
 One printable body, salvages only the existing ~M5 thumbscrew/swivel foot.

 Installed coordinates (all mm):
   X rightward; Z upward; Y toward monitor's rear.
   LCD long edge extends horizontally to the LEFT (X=0..155).
   U-channel (X=59..155) grabs monitor's BOTTOM horizontal edge,
   opening UPWARD (+Z), with its rear jaw and screw behind monitor.
   The LCD frame TOP aligns with the clamp FRONT jaw TOP.
   LCD screen hangs down below the monitor; frame never projects
   above the clamp FRONT jaw, and is fused to the front jaw.
   PCB mounting and the fragile bridge from V3 are removed.
   Thread: PRINTED right-hand helical M5x0.8 internal female thread,
   not a smooth through-hole and not an embedded nut.

 Note: draft fitting geometry. Calibrate print for thread shrinkage.
 Verify monitor thickness, thumb screw travel, PCB ribbon and USB location.
*/

$fn = 64;

// ---- Display dimensions (nominal; PCB features intentionally omitted) ----
screen_w = 155;
screen_h = 58;
screen_max_t = 6.8;
screen_black_short = 5.5; // left and right long-edge endpoints
screen_black_long = 3.5;  // top and bottom

// ---- LCD tray (front-loaded, open face, adhesive retention) ----
screen_edge_clearance = 0.5;
frame_wall = 2.3;
frame_rear_t = 3.0;
frame_rear_rim = 6.0;  // supports ONLY the non-central back edge
screen_front_recess = 0.2;
adhesive_allowance = 0.5;
// The rear LCD rim nearly meets the clamp front jaw, allowing full-width
// integrated reinforcement rather than the thin V3 joining bridge.
frame_front_y = -14.8;
frame_floor_y = frame_front_y + screen_front_recess + screen_max_t + adhesive_allowance;
frame_back_y = frame_floor_y + frame_rear_t;

// ---- Horizontal monitor BOTTOM-edge clamp ----
clamp_length = 96;
clamp_inner_gap = 38;  // Y gap between front/rear jaws (confirm on monitor!)
clamp_x_right = screen_w + screen_edge_clearance + frame_wall;
clamp_x_left = clamp_x_right-clamp_length;
clamp_front_t = 4.5;
clamp_rear_t = 6.0;
clamp_spine_t = 6.0;
clamp_spine_bottom_z = -14;
clamp_spine_top_z = clamp_spine_bottom_z+clamp_spine_t;
clamp_front_rise = 7.0;  // LOW front lip reduces obstruction of main display
clamp_rear_rise = 10.0;
clamp_rear_y = clamp_inner_gap;

// ---- V4: EXACT top alignment, zero offset ----
// The frame top and the clamp front contact lip top are coplanar.
// No PCB mounting structures and no skinny hanging connection.
front_lip_top_z = clamp_spine_top_z + clamp_front_rise;
screen_z0 = front_lip_top_z - screen_h - screen_edge_clearance - frame_wall;

// ---- Wide, directly integrated load path on FRONT SIDE of U-clamp ----
// Behind LCD only (positive Y). The top web is a deep backer plate,
// united directly to the clamp front jaw across all 96mm. Additional
// vertical ties link the web to the lower LCD frame for torsional rigidity.
// The middle/rear of the LCD remains open for a ribbon tail and local bulge.
web_depth_z = 31.0;   // deep connection, not a thin rib (mm below frame top)
web_front_y = frame_back_y - 0.8;  // slightly overlap the rear rim
web_back_y = -0.8;    // inside front face of the U-clamp; never inside clamp
reinforce_ties = true;
tie_width = 7.0;
// This is a fitting model: the rear of the screen must be checked for
// any ribbon connector interference with the integrated web or ties.

// ---- M5 x 0.8 REAL PRINTED INNER THREAD ----
// Plastic printed threads need clearance. Tune if printed result is tight.
screw_pitch = 0.8;
screw_minor_d = 4.40;    // central pilot bore / internal tooth crest
screw_major_d = 5.30;    // inside root of plastic thread, FDM allowance
screw_entry_d = 5.50;    // 0.6-mm lead-in chamfers at both ends
screw_boss_y = 10.0;     // full 16mm of material (rear jaw 6 + boss 10)
screw_boss_w = 20.0;
screw_boss_h = 18.0;
screw_x = clamp_x_left + clamp_length/2;
screw_z = clamp_spine_top_z + clamp_rear_rise/2;
thread_steps_per_turn = 18; // increase to 24 for smoother threads

// ---- FPC access notch (position approximate, adjustable) ----
fpc_right_notch = true;
fpc_notch_center_z = screen_z0 + screen_h/2;
fpc_notch_height_z = 19;

// Debug silhouettes; previews only, NEVER exported with STL.
show_placeholders = false;
part_mode = "full"; // "full" or "thread_test" (unchanged M5x0.8 coupon)

function outer_x0() = -screen_edge_clearance-frame_wall;
function outer_z0() = screen_z0-screen_edge_clearance-frame_wall;
function outer_w() = screen_w + 2*(screen_edge_clearance+frame_wall);
function outer_h() = screen_h + 2*(screen_edge_clearance+frame_wall);
function screen_frame_top() = outer_z0()+outer_h();

module at_box(x,y,z,w,d,h) { translate([x,y,z]) cube([w,d,h]); }

module lcd_frame() {
    // Back rim with open center for ribbon connector & local thickness.
    difference() {
        at_box(outer_x0(),frame_floor_y,outer_z0(),outer_w(),frame_rear_t,outer_h());
        at_box(outer_x0()+frame_rear_rim,frame_floor_y-.5,
               outer_z0()+frame_rear_rim,
               outer_w()-2*frame_rear_rim, frame_rear_t+1,
               outer_h()-2*frame_rear_rim);
    }
    // Four protective guide walls. No front bezel covering image.
    difference() {
        union() {
            at_box(outer_x0(),frame_front_y,outer_z0(),
                   frame_wall,frame_floor_y-frame_front_y,outer_h());
            at_box(screen_w+screen_edge_clearance,frame_front_y,outer_z0(),
                   frame_wall,frame_floor_y-frame_front_y,outer_h());
            at_box(-screen_edge_clearance,frame_front_y,outer_z0(),
                   screen_w+2*screen_edge_clearance,
                   frame_floor_y-frame_front_y,frame_wall);
            at_box(-screen_edge_clearance,frame_front_y,
                   screen_z0+screen_h+screen_edge_clearance,
                   screen_w+2*screen_edge_clearance,
                   frame_floor_y-frame_front_y,frame_wall);
        }
        if (fpc_right_notch)
            at_box(screen_w+screen_edge_clearance-.5,frame_front_y-1,
                   fpc_notch_center_z-fpc_notch_height_z/2,
                   frame_wall+1.5,frame_floor_y-frame_front_y+2,
                   fpc_notch_height_z);
    }
}

module clamp_body() {
    // Spine underneath the bottom monitor edge.
    at_box(clamp_x_left,-clamp_front_t,clamp_spine_bottom_z,
           clamp_length,clamp_inner_gap+clamp_front_t+clamp_rear_t,
           clamp_spine_t);
    // Front contact lip: only a short distance up the bottom bezel.
    at_box(clamp_x_left,-clamp_front_t,clamp_spine_top_z,
           clamp_length,clamp_front_t,clamp_front_rise);
    // Rear lip, thin along Z but thickened locally for threaded screw.
    at_box(clamp_x_left,clamp_rear_y,clamp_spine_top_z,
           clamp_length,clamp_rear_t,clamp_rear_rise);
    at_box(screw_x-screw_boss_w/2,clamp_rear_y+clamp_rear_t,
           screw_z-screw_boss_h/2,
           screw_boss_w,screw_boss_y,screw_boss_h);
}

// V4 direct integration of the LCD rear rim with the FULL clamp front jaw.
// The plate is directly joined across x = clamp_x_left .. clamp_x_right,
// from z = front_lip_top - 31 to z = front_lip_top. No skinny neck.
module integrated_front_web() {
    z0 = front_lip_top_z - web_depth_z;
    at_box(clamp_x_left, web_front_y, z0,
           clamp_length, web_back_y-web_front_y,
           web_depth_z);

    // Long ties reinforce the screen frame and attach the backer web to
    // its bottom supporting rim; leave the broad center clear for FPC.
    if (reinforce_ties)
        for (xx = [clamp_x_left + 5, clamp_x_right - tie_width - 5])
            at_box(xx,web_front_y,
                   outer_z0() + 2,
                   tie_width, web_back_y-web_front_y,
                   (front_lip_top_z - outer_z0() - 2));
}

// Accurate axial-pitch helix, modelled as a watertight swept cutter.
// The 4-point cross section is defined in the radius/axial plane.
// The female groove is cut by moving the cutter one pitch per revolution.
function thread_r(j) =
    j==0 || j==3 ? screw_minor_d/2-0.10 : screw_major_d/2;
function thread_axial(j) =
    j==0 ? -0.40*screw_pitch :
    j==1 ? -0.09*screw_pitch :
    j==2 ?  0.09*screw_pitch : 0.40*screw_pitch;

module helical_thread_cutter(thread_len) {
    steps = ceil(thread_len/screw_pitch*thread_steps_per_turn);
    points = [ for(i=[0:steps]) for(j=[0:3])
        let(a=360*thread_len*i/(steps*screw_pitch),
            z=thread_len*i/steps+thread_axial(j),
            r=thread_r(j))
        [r*cos(a),r*sin(a),z]
    ];
    // Triangulated side faces avoid non-planar helicoid quads.
    faces = concat(
        [for(i=[0:steps-1])for(j=[0:3])
            [4*i+j,4*(i+1)+j,4*(i+1)+(j+1)%4]],
        [for(i=[0:steps-1])for(j=[0:3])
            [4*i+j,4*(i+1)+(j+1)%4,4*i+(j+1)%4]],
        [[0,1,2],[0,2,3]],
        [[4*steps+3,4*steps+2,4*steps+1],
         [4*steps+3,4*steps+1,4*steps]]
    );
    polyhedron(points=points,faces=faces,convexity=20);
}

module threaded_m5_hole() {
    full_t = clamp_rear_t+screw_boss_y;
    // Rotate local Z to global +Y; this remains a RIGHT-HAND thread.
    translate([screw_x,clamp_rear_y-1,screw_z])
    rotate([-90,0,0])
    union() {
        cylinder(d=screw_minor_d,h=full_t+2,$fn=72);
        helical_thread_cutter(full_t+2);
        translate([0,0,0.9])
            cylinder(d1=screw_entry_d,d2=screw_minor_d,h=0.7);
        translate([0,0,full_t+0.4])
            cylinder(d1=screw_minor_d,d2=screw_entry_d,h=0.7);
    }
}

module printable_part() {
    difference() {
        union() {
            lcd_frame();
            clamp_body();
            integrated_front_web();
        }
        threaded_m5_hole();
    }
}

module m5_thread_test_coupon() {
    // Small trial print BEFORE the long one-piece clamp.
    difference() {
        at_box(screw_x-9,clamp_rear_y,screw_z-9,
               18,clamp_rear_t+screw_boss_y,18);
        threaded_m5_hole();
    }
}

difference() { clamp_body(); threaded_m5_hole(); }
