#include <MOE/MOE.hpp> // road map to other files including in program, do not go through recursively
#include <Mahi/Com.hpp>
#include <Mahi/Util.hpp>
#include <Mahi/Daq.hpp>
#include <Mahi/Robo.hpp>
#include <vector> 

using namespace mahi::util; // naming convention for variables, able to access outside of namespace
using namespace mahi::daq;
using namespace mahi::robo;
using namespace mahi::com;
using namespace moe;
using namespace std;
  
using mahi::robo::WayPoint;

// DOG setting order, amount of time @ each state, defining 'things' that will be used throughout program
enum state {          // enum = evaluate as integers, unscoped enumerations
    to_neutral_0,     // 0
    to_top_elbow,     // 1
    to_neutral_1,     // 2
};

// create global stop variable CTRL-C han dler function
ctrl_bool stop(false);
bool handler(CtrlEvent event) {
    stop = true;
    return true;
}

// MinimumJerk Traj.: (3rd derivate) more smooth for comfortability of user, used frequently in robotics
void to_state(state& current_state_, const state next_state_, WayPoint current_position_, WayPoint new_position_, Time traj_length_, MinimumJerk& mj_, Clock& ref_traj_clock_) {
    current_position_.set_time(seconds(0));
    new_position_.set_time(traj_length_);
    mj_.set_endpoints(current_position_, new_position_);
    
    if (!mj_.trajectory().validate()) {
        LOG(Warning) << "Minimum Jerk trajectory invalid.";
        stop = true;
    }
    current_state_ = next_state_;
    ref_traj_clock_.restart();
} 

int main(int argc, char* argv[]) {
    // register ctrl-c handler
    register_ctrl_handler(handler);

    // make options (terminal)
    Options options("ex_skye_demo", "Skye's first go on writing trajectory");
    options.add_options()
		("c,calibrate", "Calibrates the MAHI Exo-II")
        ("n,no_torque", "trajectories are generated, but not torque provided")
        ("v,virtual", "example is virtual and will communicate with the unity sim")
		("h,help", "Prints this help message");

    auto result = options.parse(argc, argv);

    // if -h, print the help option
    if (result.count("help") > 0) {
        print_var(options.help());
        return 0;
    }

    // enable Windows realtime
    enable_realtime();

    Time Ts = milliseconds(1);  // sample period for DAQ

    /////////////////////////////////
    // construct and config MOE   //
    /////////////////////////////////

    // making DAQ, using command line arguments, connect to DAQ & MOE, turn on/off motors

    std::shared_ptr<MahiOpenExo> moe = nullptr;
    std::shared_ptr<Q8Usb> daq = nullptr;
    
    // set up to either connect to the sim or the real robot
    if(result.count("virtual") > 0){
        MoeConfigurationVirtual config_vr; 
        moe = std::make_shared<MahiOpenExoVirtual>(config_vr);
    }
    else{
        daq = std::make_shared<Q8Usb>();
        daq->open();

        MoeConfigurationHardware config_hw(*daq,VelocityEstimator::Hardware); 

        // # of daq channels
        std::vector<TTL> idle_values(8,TTL_LOW);
        daq->DO.enable_values.set({0,1,2,3,4,5,6,7},idle_values);
        daq->DO.disable_values.set({0,1,2,3,4,5,6,7},idle_values);
        daq->DO.expire_values.write({0,1,2,3,4,5,6,7},idle_values);   

        moe = std::make_shared<MahiOpenExoHardware>(config_hw);
    }

    ////////////////////////////////

    //////////////////////////////////////////////
    // create MahiOpenExo and bind daq channels to it
    //////////////////////////////////////////////

    bool rps_is_init = false;

    //////////////////////////////////////////////

    // calibrate - manually zero the encoders (right arm supinated)
    if (result.count("calibrate") > 0) {
        moe->calibrate_auto(stop);
        LOG(Info) << "MAHI Exo-II encoders calibrated.";
        return 0; // end code early 
    }

    // make MelShares
    MelShare ms_pos("ms_pos");
    MelShare ms_vel("ms_vel");
    MelShare ms_trq("ms_trq");
    MelShare ms_ref("ms_ref");

    // create ranges for saturating trajectories for safety  MIN            MAX
    std::vector<std::vector<double>> setpoint_rad_ranges = {{-90 * DEG2RAD, 20 * DEG2RAD}, // max & min for each DOF
                                                            {-90 * DEG2RAD, 90 * DEG2RAD},
                                                            {-80 * DEG2RAD, 80 * DEG2RAD},
                                                            {-60 * DEG2RAD, 60 * DEG2RAD}};

    std::vector<Time> state_times = {seconds(2.0),  // to_neutral_0
                                     seconds(3.0),  // to_top_elbow
                                     seconds(4.0)};  // to_neutral_1

    // setup trajectories
    double t = 0;

    Time mj_Ts = milliseconds(50);

    std::vector<double> ref;
   
    // waypoints = location markers, obtain coordinate data for locations, different points tring to use throughout experiment, allocate when & how long 
    // waypoints                                   Elbow F/E       Forearm P/S   Wrist F/E     Wrist R/U     LastDoF
    WayPoint neutral_point = WayPoint(Time::Zero, {-15 * DEG2RAD,  00 * DEG2RAD, 00  * DEG2RAD, 00 * DEG2RAD});
    WayPoint bottom_elbow  = WayPoint(Time::Zero, {-65 * DEG2RAD,  45 * DEG2RAD, 30  * DEG2RAD, 00 * DEG2RAD});
    WayPoint top_elbow     = WayPoint(Time::Zero, { 20 * DEG2RAD,  45 * DEG2RAD, 00  * DEG2RAD, 15 * DEG2RAD});
    WayPoint top_wrist     = WayPoint(Time::Zero, { 00 * DEG2RAD,  00 * DEG2RAD, 00  * DEG2RAD, 00 * DEG2RAD});

    // construct timer in hybrid mode to avoid using 100% CPU - (kH) important for pausing, etc.
    Timer timer(Ts, Timer::Hybrid); 
    timer.set_acceptable_miss_rate(0.05);

// construct clock for regulating keypress
    Clock keypress_refract_clock;
    Time keypress_refract_time = seconds(0.5);

    std::vector<std::string> dof_str = {"ElbowFE", "WristPS", "WristFE", "WristRU"};

    ////////////////////////////////////////////////
    //////////// State Manager Setup ///////////////
    ////////////////////////////////////////////////

    state current_state = to_neutral_0;
    WayPoint current_position;
    WayPoint new_position; 
    Time traj_length;
    WayPoint dummy_waypoint = WayPoint(Time::Zero, {-35 * DEG2RAD,  00 * DEG2RAD, 00  * DEG2RAD, 00 * DEG2RAD});
    MinimumJerk mj(mj_Ts, dummy_waypoint, neutral_point.set_time(state_times[to_neutral_0]));
    std::vector<double> traj_max_diff = { 60 * DEG2RAD, 60 * DEG2RAD, 100 * DEG2RAD, 60 * DEG2RAD};
	mj.set_trajectory_params(Trajectory::Interp::Linear, traj_max_diff);
    Clock ref_traj_clock;

    // traj_max_diff = maximum velocity allowed to have, checks trajectory, checks to make sure 
    std::vector<double> aj_positions(4,0.0);
    std::vector<double> aj_velocities(4,0.0);

    std::vector<double> command_torques(4,0.0);

    ref_traj_clock.restart();

    // enable DAQ and exo
	moe->daq_enable();
	
    moe->enable();
	
	// moe->daq_watchdog_start();    

    // trajectory following
    LOG(Info) << "Starting Movement.";

    std::vector<std::vector<double>> data;
    std::vector<double> data_line;

    //initialize kinematics
    moe->daq_read_all();
    moe->update();

    WayPoint start_pos(Time::Zero, moe->get_joint_positions());
//        cout << "Checkpoint 1"<<endl;

    mj.set_endpoints(start_pos, neutral_point.set_time(state_times[to_neutral_0]));
//        cout << "Checkpoint 2"<<endl;

 // constrain trajectory to be within range
       while (!stop) {
        // update all DAQ input channels
        moe->daq_read_all();
        // update MahiOpenExo kinematics
        moe->update();
        // update reference from trajectory
        ref = mj.trajectory().at_time(ref_traj_clock.get_elapsed_time());
    
//        cout << "Checkpoint 3"<<endl;
//        cout << ref << endl;

        for (std::size_t i = 0; i < moe->n_j; ++i) {
             ref[i] = clamp(ref[i], setpoint_rad_ranges[i][0], setpoint_rad_ranges[i][1]);
        }

//        cout << "Checkpoint 4"<<endl;

        // calculate anatomical command torques
        if (result.count("no_torque") > 0){
            command_torques = {0.0, 0.0, 0.0, 0.0, 0.0};
            moe->set_raw_joint_torques(command_torques);
            // cout << command_torques ;
        }
        else{
            command_torques = moe->set_pos_ctrl_torques(ref);
           // cout << command_torques ;
        }
//        cout << command_torques << endl ;
//        cout << state_times[current_state] << endl;
//        cout << ref_traj_clock.get_elapsed_time() << endl;

         // if enough time has passed, continue to the next state. See to_state function at top of file for details
        if (ref_traj_clock.get_elapsed_time() > state_times[current_state]) {
//            cout << "Checkpoint 5"<<endl;
            switch (current_state) {
                case to_neutral_0:
                             // cur state    next state        curr pos      next pos       time
                    to_state(current_state, to_top_elbow, neutral_point, top_elbow, state_times[to_top_elbow], mj, ref_traj_clock);
                    // moe->set_high_gains();
                    break;
//                    cout << "Checkpoint 6"<<endl;
                case to_top_elbow:
                    to_state(current_state, to_neutral_1, top_elbow, neutral_point, state_times[to_neutral_1], mj, ref_traj_clock);
                    break;
//                    cout << "Checkpoint 7"<<endl;
                case to_neutral_1:
                    stop = true;
                    break;
            }
        }
//        cout << "Checkpoint 8"<<endl;
               

         std::vector<double> act_torque;
        if (!result.count("virtual")) {
            daq->AI.read();
            act_torque = {daq->AI[0],daq->AI[1],daq->AI[2],daq->AI[3]};

        }
        else{
            act_torque = {0,0,0,0};
        }
        std::vector<double> grav_torques =  moe->calc_grav_torques();
        data_line.clear();
        data_line.push_back(t);
        for (const auto &i : ref) data_line.push_back(i);
        for (const auto &i : moe->get_joint_positions()) data_line.push_back(i);
        for (const auto &i : moe->get_joint_velocities()) data_line.push_back(i);
        for (const auto &i : moe->get_joint_command_torques()) data_line.push_back(i);
        for (const auto &trq : act_torque) data_line.push_back(trq);
        data.push_back(data_line);
        
        // kick watchdog
        // if (!moe->daq_watchdog_kick() || moe->any_limit_exceeded()) {
        //     stop = true;
        // }
        if (moe->any_limit_exceeded()) {
            stop = true;
        }

        // update all DAQ output channels
        if (!stop) moe->daq_write_all();

        ms_ref.write_data(moe->get_joint_positions());
        ms_pos.write_data(moe->get_joint_velocities());

        // wait for remainder of sample period
        t = timer.wait().as_seconds();
       } 
    command_torques = {0.0, 0.0, 0.0, 0.0, 0.0};
    moe->set_raw_joint_torques(command_torques);
    moe->daq_write_all();

    std::vector<std::string> header = {"Time (s)", 
                                       "EFE ref (rad)", "FPS ref (rad)", "WFE ref (rad)", "WRU ref (rad)",
                                       "EFE act (rad)", "FPS act (rad)", "WFE act (rad)", "WRU act (rad)",
                                       "EFE act (rad/s)", "FPS act (rad/s)", "WFE act (rad/s)", "WRU act (rad/s)",
                                       "EFE trq (Nm)", "FPS trq (Nm)", "WFE trq (Nm)", "WRU trq (Nm)",
                                       "EFE act trq (Nm)","FPS act trq (Nm)", "WFE act trq (Nm)", "WRU act trq (Nm)"};

    csv_write_row("data/skye_is_cool_results.csv",header);
    csv_append_rows("data/skye_is_cool_results.csv",data);
    
    moe->daq_disable();
    moe->disable();

    disable_realtime();

    // clear console buffer
    while (get_key_nb() != 0);

    return 0;
}