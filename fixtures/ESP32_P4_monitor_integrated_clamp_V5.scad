/*
 ESP32-P4 monitor clamp V5 -- measured 160 x 59.3 x 0.17 mm panel.
 X right, Y toward monitor rear, Z up. Board upper-left is defined when
 looking at the SCREEN FRONT; rear-view drawings therefore mirror X.
 V4 clamp, M5 thread, screw position and WORLD coordinates are unchanged.
 Only the screen frame, its connection and PCB support are revised.
 All dimensions in mm. This is a fit prototype, not a tested assembly.
*/
$fn = 64;

screen_w = 160;
screen_h = 59.3;
screen_t = 0.17;           // literal measured panel thickness, NOT 1.7 mm
screen_edge_clearance = 0.5; // per side: opening 161 x 60.3
frame_wall = 2.3;
screen_front_recess = 0.2;
adhesive_allowance = 0.2;

board_w = 99;
board_h = 45.4;
board_max_t = 5.21;       // inherited assembly envelope; editable
board_x0 = 0;
board_standoff = 2.0;    // gap from backplate to PCB rear
board_clamp_clearance = 1.0;
backplate_t = 3.0;
board_hole_nominal_d = 3.0;
board_hole_edge_gap = 2.0;
board_hole_inset = board_hole_edge_gap + board_hole_nominal_d/2;
board_screw_clearance_d = 3.2;
board_boss_d = 6.4;
board_nut_af = 5.7;      // M3 hex nut pocket, across flats
board_nut_depth = 2.5;   // pocket opens on SCREEN side; nut must sit flush

// V4 CLAMP DATUM: preserve positions, do NOT derive from new screen width.
clamp_length = 96;
clamp_inner_gap = 38;
clamp_x_right = 157.8;
clamp_x_left = clamp_x_right-clamp_length;
clamp_front_t = 4.5;
clamp_rear_t = 6.0;
clamp_spine_t = 6.0;
clamp_spine_bottom_z = -14;
clamp_spine_top_z = clamp_spine_bottom_z+clamp_spine_t;
clamp_front_rise = 7.0;
clamp_rear_rise = 10.0;
clamp_rear_y = clamp_inner_gap;
front_lip_top_z = clamp_spine_top_z + clamp_front_rise;
screen_z0 = front_lip_top_z-screen_h-screen_edge_clearance-frame_wall;
board_top_z = screen_z0+screen_h;
board_bottom_z = board_top_z-board_h;

// Place the full PCB envelope ahead of the unchanged V4 front jaw.
// Components face away from the panel, toward +Y. No board/clamp overlap.
board_rearmost_y = -clamp_front_t-board_clamp_clearance;
board_mount_y = board_rearmost_y-board_max_t;
frame_back_y = board_mount_y-board_standoff;
frame_floor_y = frame_back_y-backplate_t;
frame_front_y = frame_floor_y-adhesive_allowance-screen_t-screen_front_recess;
// Panel pocket depth is ONLY 0.57; the side ring continues behind it.
// There is NO 6-mm rear supporting rim and NO internal peripheral ledge.

// V4 M5 thread parameters -- unmodified.
screw_pitch = 0.8;
screw_minor_d = 4.40;
screw_major_d = 5.30;
screw_entry_d = 5.50;
screw_boss_y = 10.0;
screw_boss_w = 20.0;
screw_boss_h = 18.0;
screw_x = clamp_x_left + clamp_length/2;
screw_z = clamp_spine_top_z + clamp_rear_rise/2;
thread_steps_per_turn = 18;

show_placeholders = false;
part_mode = "full"; // full, thread_test, clamp_only, frame_only, backplate_only

function outer_x0() = -screen_edge_clearance-frame_wall;
function outer_z0() = screen_z0-screen_edge_clearance-frame_wall;
function outer_w() = screen_w+2*(screen_edge_clearance+frame_wall);
function outer_h() = screen_h+2*(screen_edge_clearance+frame_wall);
function screen_frame_top() = outer_z0()+outer_h();
function hole_xs() = [board_x0+board_hole_inset,
                       board_x0+board_w-board_hole_inset];
function hole_z() = board_bottom_z+board_hole_inset;
module at_box(x,y,z,w,d,h) { translate([x,y,z]) cube([w,d,h]); }
module y_cylinder(x,y,z,d,h,n=64) {
    translate([x,y,z]) rotate([-90,0,0]) cylinder(d=d,h=h,$fn=n);
}

module lcd_frame() {
    // Constant 161 x 60.3 opening all the way through. No right FPC notch.
    difference() {
        at_box(outer_x0(),frame_front_y,outer_z0(),
               outer_w(),frame_back_y-frame_front_y,outer_h());
        at_box(-screen_edge_clearance,frame_front_y-1,
               screen_z0-screen_edge_clearance,
               screen_w+2*screen_edge_clearance,
               frame_back_y-frame_front_y+2,
               screen_h+2*screen_edge_clearance);
    }
}

module pcb_backplate() {
    // Plate stays within the PCB outline except 0.7-mm joins to the left
    // and upper frame walls. Screen outside PCB footprint stays open.
    difference() {
        union() {
            at_box(-0.7,frame_floor_y,board_bottom_z,
                   board_w+0.7,backplate_t,
                   screen_z0+screen_h+screen_edge_clearance+0.2-board_bottom_z);
            // Two lower screw bosses plus two upper supporting pads.
            for (xx=hole_xs()) {
                y_cylinder(xx,frame_back_y-0.1,hole_z(),
                           board_boss_d,board_standoff+0.1);
                y_cylinder(xx,frame_back_y-0.1,
                           board_top_z-board_hole_inset,
                           board_boss_d,board_standoff+0.1);
            }
        }
        for (xx=hole_xs()) {
            y_cylinder(xx,frame_floor_y-0.1,hole_z(),
                       board_screw_clearance_d,
                       backplate_t+board_standoff+0.2);
            // Orient flat faces along Z; fits inside the board footprint.
            translate([xx,frame_floor_y-0.1,hole_z()])
                rotate([-90,0,0]) rotate([0,0,30])
                cylinder(d=board_nut_af/cos(30),
                         h=board_nut_depth+0.1,$fn=6);
        }
    }
}

module frame_top_connection() {
    // Only the EXISTING top frame border carries the bridge to the jaw.
    // The former 31-mm web and both long vertical ties are removed.
    at_box(clamp_x_left,frame_back_y-0.2,
           screen_z0+screen_h+screen_edge_clearance,
           clamp_length,-clamp_front_t+0.2-(frame_back_y-0.2),frame_wall);
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
            pcb_backplate();
            frame_top_connection();
            clamp_body();
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

if (part_mode == "thread_test") m5_thread_test_coupon();
else if (part_mode == "clamp_only")
    difference() { clamp_body(); threaded_m5_hole(); }
else if (part_mode == "frame_only") lcd_frame();
else if (part_mode == "backplate_only") pcb_backplate();
else printable_part();

if (show_placeholders && part_mode == "full") {
    color([0.15,0.6,0.9,0.4])
    %at_box(0,frame_front_y+screen_front_recess,screen_z0,
            screen_w,screen_t,screen_h);
    color([0.1,0.7,0.35,0.5])
    %at_box(board_x0,board_mount_y,board_bottom_z,
            board_w,board_max_t,board_h);
}
