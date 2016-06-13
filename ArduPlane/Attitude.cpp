// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include "Plane.h"

/*
  get a speed scaling number for control surfaces. This is applied to
  PIDs to change the scaling of the PID with speed. At high speed we
  move the surfaces less, and at low speeds we move them more.
 */
float Plane::get_speed_scaler(void)
{
    float aspeed, speed_scaler;
    if (ahrs.airspeed_estimate(&aspeed)) {
        if (aspeed > auto_state.highest_airspeed) {
            auto_state.highest_airspeed = aspeed;
        }
        if (aspeed > 0) {
            speed_scaler = g.scaling_speed / aspeed;
        } else {
            speed_scaler = 2.0;
        }
        speed_scaler = constrain_float(speed_scaler, 0.5f, 2.0f);
    } else {
        if (channel_throttle->servo_out > 0) {
            speed_scaler = 0.5f + ((float)THROTTLE_CRUISE / channel_throttle->servo_out / 2.0f);                 // First order taylor expansion of square root
            // Should maybe be to the 2/7 power, but we aren't goint to implement that...
        }else{
            speed_scaler = 1.67f;
        }
        // This case is constrained tighter as we don't have real speed info
        speed_scaler = constrain_float(speed_scaler, 0.6f, 1.67f);
    }
    return speed_scaler;
}

/*
  return true if the current settings and mode should allow for stick mixing
 */
bool Plane::stick_mixing_enabled(void)
{
    if (auto_throttle_mode) {
        // we're in an auto mode. Check the stick mixing flag
        if (g.stick_mixing != STICK_MIXING_DISABLED &&
            geofence_stickmixing() &&
            failsafe.state == FAILSAFE_NONE &&
            !rc_failsafe_active()) {
            // we're in an auto mode, and haven't triggered failsafe
            return true;
        } else {
            return false;
        }
    }

    if (failsafe.ch3_failsafe && g.short_fs_action == 2) {
        // don't do stick mixing in FBWA glide mode
        return false;
    }

    // non-auto mode. Always do stick mixing
    return true;
}


/*
  this is the main roll stabilization function. It takes the
  previously set nav_roll calculates roll servo_out to try to
  stabilize the plane at the given roll
 */
void Plane::stabilize_roll(float speed_scaler)
{
    if (fly_inverted()) {
        // we want to fly upside down. We need to cope with wrap of
        // the roll_sensor interfering with wrap of nav_roll, which
        // would really confuse the PID code. The easiest way to
        // handle this is to ensure both go in the same direction from
        // zero
        nav_roll_cd += 18000;
        if (ahrs.roll_sensor < 0) nav_roll_cd -= 36000;
    }

    bool disable_integrator = false;
  //  if (control_mode == STABILIZE && channel_roll->control_in != 0) {
  //    disable_integrator = true;
  //}
    channel_roll->servo_out = rollController.get_servo_out(nav_roll_cd - ahrs.roll_sensor, 
                                                           speed_scaler, 
                                                           disable_integrator);
}

/*
  this is the main pitch stabilization function. It takes the
  previously set nav_pitch and calculates servo_out values to try to
  stabilize the plane at the given attitude.
 */
void Plane::stabilize_pitch(float speed_scaler)
{
    int8_t force_elevator = takeoff_tail_hold();
    if (force_elevator != 0) {
        // we are holding the tail down during takeoff. Just covert
        // from a percentage to a -4500..4500 centidegree angle
        channel_pitch->servo_out = 45*force_elevator;
        return;
    }
    int32_t demanded_pitch = nav_pitch_cd + g.pitch_trim_cd + channel_throttle->servo_out * g.kff_throttle_to_pitch;
    bool disable_integrator = false;
 //   if (control_mode == STABILIZE && channel_pitch->control_in != 0) {
 //       disable_integrator = true;
 //   }  
  //original file STABILIAZE's I doesn't work,I comment it to let it also integrate
    channel_pitch->servo_out = pitchController.get_servo_out(demanded_pitch - ahrs.pitch_sensor, 
                                                             speed_scaler, 
                                                             disable_integrator);

    if (control_mode == STABILIZE ||
    	control_mode == JULAND)
    {   
    	if (jinit_counter ==0) {
           pitch_servo_out_init1 = channel_pitch->servo_out ;
           channel_pitch->servo_out = pitch_servo_out_init1;
         }
        if (jinit_counter <= g.JU_init_transtime) {
           channel_pitch->servo_out = (1-jinit_counter/g.JU_init_transtime) * pitch_servo_out_init1 + (jinit_counter/g.JU_init_transtime)*g.JU_pitch_ser01 + channel_pitch->servo_out*(jinit_counter/g.JU_init_transtime);
         }
        else {

           if (ju_flarestage == 0) {
              channel_pitch->servo_out += g.JU_pitch_ser01;
             }
           else {
              channel_pitch->servo_out += g.JU_pitch_ser02;
             }
         }



//        if (ju_flarestage == 1) {
//           if(jthoflare_counter == 0) {
//              pitch_servo_out_init2 = channel_pitch->servo_out ;
//           }
//           if(jthoflare_counter<=g.JU_tho_flaret) {
//           	  channel_pitch->servo_out = (1-jthoflare_counter/g.JU_tho_flaret) * pitch_servo_out_init2 + (jthoflare_counter/g.JU_tho_flaret)*g.JU_pitch_ser02 + channel_pitch->servo_out*(jthoflare_counter/g.JU_tho_flaret);
//           }
//           else {
//           channel_pitch->servo_out += g.JU_pitch_ser02;
//           }
//        }
    }
}

/*
  perform stick mixing on one channel
  This type of stick mixing reduces the influence of the auto
  controller as it increases the influence of the users stick input,
  allowing the user full deflection if needed
 */
void Plane::stick_mix_channel(RC_Channel *channel, int16_t &servo_out)
{
    float ch_inf;
        
    ch_inf = (float)channel->radio_in - (float)channel->radio_trim;
    ch_inf = fabsf(ch_inf);
    ch_inf = MIN(ch_inf, 400.0f);
    ch_inf = ((400.0f - ch_inf) / 400.0f);
    servo_out *= ch_inf;
    servo_out += channel->pwm_to_angle();
}

/*
  this gives the user control of the aircraft in stabilization modes
 */
void Plane::stabilize_stick_mixing_direct()
{
    if (!stick_mixing_enabled() ||
        control_mode == ACRO ||
        control_mode == FLY_BY_WIRE_A ||
        control_mode == AUTOTUNE ||
        control_mode == FLY_BY_WIRE_B ||
        control_mode == CRUISE ||
        control_mode == QSTABILIZE ||
        control_mode == QHOVER ||
        control_mode == QLOITER ||
        control_mode == TRAINING ||
        control_mode == STABILIZE) {
        return;
    }
    stick_mix_channel(channel_roll, channel_roll->servo_out);
    stick_mix_channel(channel_pitch, channel_pitch->servo_out);
}

/*
  this gives the user control of the aircraft in stabilization modes
  using FBW style controls
 */
void Plane::stabilize_stick_mixing_fbw()
{
    if (!stick_mixing_enabled() ||
        control_mode == ACRO ||
        control_mode == FLY_BY_WIRE_A ||
        control_mode == AUTOTUNE ||
        control_mode == FLY_BY_WIRE_B ||
        control_mode == CRUISE ||
        control_mode == QSTABILIZE ||
        control_mode == QHOVER ||
        control_mode == QLOITER ||
        control_mode == TRAINING ||
        (control_mode == AUTO && g.auto_fbw_steer)) {
        return;
    }
    // do FBW style stick mixing. We don't treat it linearly
    // however. For inputs up to half the maximum, we use linear
    // addition to the nav_roll and nav_pitch. Above that it goes
    // non-linear and ends up as 2x the maximum, to ensure that
    // the user can direct the plane in any direction with stick
    // mixing.
    float roll_input = channel_roll->norm_input_dz();
    if (roll_input > 0.5f) {
        roll_input = (3*roll_input - 1);
    } else if (roll_input < -0.5f) {
        roll_input = (3*roll_input + 1);
    }
    nav_roll_cd += roll_input * roll_limit_cd;
    nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit_cd, roll_limit_cd);
    
    float pitch_input = channel_pitch->norm_input_dz();  // change norm_input() to norm_input_dz() to avoid trim change 
    if (fabsf(pitch_input) > 0.5f) {
        pitch_input = (3*pitch_input - 1);
    }
    if (fly_inverted()) {
        pitch_input = -pitch_input;
    }
    if (pitch_input > 0) {
        nav_pitch_cd += pitch_input * aparm.pitch_limit_max_cd;
    } else {
        nav_pitch_cd += -(pitch_input * pitch_limit_min_cd);
    }
    nav_pitch_cd = constrain_int32(nav_pitch_cd, pitch_limit_min_cd, aparm.pitch_limit_max_cd.get());
}


/*
  stabilize the yaw axis. There are 3 modes of operation:

    - hold a specific heading with ground steering
    - rate controlled with ground steering
    - yaw control for coordinated flight    
 */
void Plane::stabilize_yaw(float speed_scaler)
{
    if (control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) {
        // in land final setup for ground steering
        steering_control.ground_steering = true;
    } else {
        // otherwise use ground steering when no input control and we
        // are below the GROUND_STEER_ALT
        steering_control.ground_steering = (channel_roll->control_in == 0 && 
                                            fabsf(relative_altitude()) < g.ground_steer_alt);
        if (control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_APPROACH) {
            // don't use ground steering on landing approach
            steering_control.ground_steering = false;
        }
    }


    /*
      first calculate steering_control.steering for a nose or tail
      wheel.
      We use "course hold" mode for the rudder when either in the
      final stage of landing (when the wings are help level) or when
      in course hold in FBWA mode (when we are below GROUND_STEER_ALT)
     */
    if ((control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) ||
        (steer_state.hold_course_cd != -1 && steering_control.ground_steering)) {
        calc_nav_yaw_course();
    } else if (steering_control.ground_steering) {
        calc_nav_yaw_ground();
    }

    /*
      now calculate steering_control.rudder for the rudder
     */
    calc_nav_yaw_coordinated(speed_scaler);
}


/*
  a special stabilization function for training mode
 */
void Plane::stabilize_training(float speed_scaler)
{
    if (training_manual_roll) {
        channel_roll->servo_out = channel_roll->control_in;
    } else {
        // calculate what is needed to hold
        stabilize_roll(speed_scaler);
        if ((nav_roll_cd > 0 && channel_roll->control_in < channel_roll->servo_out) ||
            (nav_roll_cd < 0 && channel_roll->control_in > channel_roll->servo_out)) {
            // allow user to get out of the roll
            channel_roll->servo_out = channel_roll->control_in;            
        }
    }

    if (training_manual_pitch) {
        channel_pitch->servo_out = channel_pitch->control_in;
    } else {
        stabilize_pitch(speed_scaler);
        if ((nav_pitch_cd > 0 && channel_pitch->control_in < channel_pitch->servo_out) ||
            (nav_pitch_cd < 0 && channel_pitch->control_in > channel_pitch->servo_out)) {
            // allow user to get back to level
            channel_pitch->servo_out = channel_pitch->control_in;            
        }
    }

    stabilize_yaw(speed_scaler);
}


/*
  this is the ACRO mode stabilization function. It does rate
  stabilization on roll and pitch axes
 */
void Plane::stabilize_acro(float speed_scaler)
{
    float roll_rate = (channel_roll->control_in/4500.0f) * g.acro_roll_rate;
    float pitch_rate = (channel_pitch->control_in/4500.0f) * g.acro_pitch_rate;

    /*
      check for special roll handling near the pitch poles
     */
    if (g.acro_locking && is_zero(roll_rate)) {
        /*
          we have no roll stick input, so we will enter "roll locked"
          mode, and hold the roll we had when the stick was released
         */
        if (!acro_state.locked_roll) {
            acro_state.locked_roll = true;
            acro_state.locked_roll_err = 0;
        } else {
            acro_state.locked_roll_err += ahrs.get_gyro().x * G_Dt;
        }
        int32_t roll_error_cd = -ToDeg(acro_state.locked_roll_err)*100;
        nav_roll_cd = ahrs.roll_sensor + roll_error_cd;
        // try to reduce the integrated angular error to zero. We set
        // 'stabilze' to true, which disables the roll integrator
        channel_roll->servo_out  = rollController.get_servo_out(roll_error_cd,
                                                                speed_scaler,
                                                                true);
    } else {
        /*
          aileron stick is non-zero, use pure rate control until the
          user releases the stick
         */
        acro_state.locked_roll = false;
        channel_roll->servo_out  = rollController.get_rate_out(roll_rate,  speed_scaler);
    }

    if (g.acro_locking && is_zero(pitch_rate)) {
        /*
          user has zero pitch stick input, so we lock pitch at the
          point they release the stick
         */
        if (!acro_state.locked_pitch) {
            acro_state.locked_pitch = true;
            acro_state.locked_pitch_cd = ahrs.pitch_sensor;
        }
        // try to hold the locked pitch. Note that we have the pitch
        // integrator enabled, which helps with inverted flight
        nav_pitch_cd = acro_state.locked_pitch_cd;
        channel_pitch->servo_out  = pitchController.get_servo_out(nav_pitch_cd - ahrs.pitch_sensor,
                                                                  speed_scaler,
                                                                  false);
    } else {
        /*
          user has non-zero pitch input, use a pure rate controller
         */
        acro_state.locked_pitch = false;
        channel_pitch->servo_out = pitchController.get_rate_out(pitch_rate, speed_scaler);
    }

    /*
      manual rudder for now
     */
    steering_control.steering = steering_control.rudder = rudder_input;
}

/*
  main stabilization function for all 3 axes
 */
void Plane::stabilize()
{
    if (control_mode == MANUAL) {
        // nothing to do
        return;
    }
    float speed_scaler = get_speed_scaler();

    if (control_mode == TRAINING) {
        stabilize_training(speed_scaler);
    } else if (control_mode == ACRO) {
        stabilize_acro(speed_scaler);
    } else if (control_mode == QSTABILIZE ||
               control_mode == QHOVER ||
               control_mode == QLOITER) {
        quadplane.control_run();
    } else {
        if (g.stick_mixing == STICK_MIXING_FBW) {
            stabilize_stick_mixing_fbw();
        }
        stabilize_roll(speed_scaler);
        stabilize_pitch(speed_scaler);
        if (g.stick_mixing == STICK_MIXING_DIRECT) {
            stabilize_stick_mixing_direct();
        }
        stabilize_yaw(speed_scaler);
    }

    /*
      see if we should zero the attitude controller integrators. 
     */
    if (channel_throttle->control_in == 0 &&
        relative_altitude_abs_cm() < 500 && 
        fabsf(barometer.get_climb_rate()) < 0.5f &&
        gps.ground_speed() < 3) {
        // we are low, with no climb rate, and zero throttle, and very
        // low ground speed. Zero the attitude controller
        // integrators. This prevents integrator buildup pre-takeoff.
        rollController.reset_I();
        pitchController.reset_I();
        yawController.reset_I();

        // if moving very slowly also zero the steering integrator
        if (gps.ground_speed() < 1) {
            steerController.reset_I();            
        }
    }
}


void Plane::calc_throttle()
{
    if (aparm.throttle_cruise <= 1) {
        // user has asked for zero throttle - this may be done by a
        // mission which wants to turn off the engine for a parachute
        // landing
        channel_throttle->servo_out = 0;
        return;
    }

    channel_throttle->servo_out = SpdHgt_Controller->get_throttle_demand();
}

/*****************************************
* Calculate desired roll/pitch/yaw angles (in medium freq loop)
*****************************************/

/*
  calculate yaw control for coordinated flight
 */
void Plane::calc_nav_yaw_coordinated(float speed_scaler)
{
    bool disable_integrator = false;
    if (control_mode == STABILIZE && rudder_input != 0) {
        disable_integrator = true;
    }
    steering_control.rudder = yawController.get_servo_out(speed_scaler, disable_integrator);

    // add in rudder mixing from roll
    steering_control.rudder += channel_roll->servo_out * g.kff_rudder_mix;
    steering_control.rudder += rudder_input;
    steering_control.rudder = constrain_int16(steering_control.rudder, -4500, 4500);
}

/*
  calculate yaw control for ground steering with specific course
 */
void Plane::calc_nav_yaw_course(void)
{
    // holding a specific navigation course on the ground. Used in
    // auto-takeoff and landing
    int32_t bearing_error_cd = nav_controller->bearing_error_cd();
    steering_control.steering = steerController.get_steering_out_angle_error(bearing_error_cd);
    if (stick_mixing_enabled()) {
        stick_mix_channel(channel_rudder, steering_control.steering);
    }
    steering_control.steering = constrain_int16(steering_control.steering, -4500, 4500);
}

/*
  calculate yaw control for ground steering
 */
void Plane::calc_nav_yaw_ground(void)
{
    if (gps.ground_speed() < 1 && 
        channel_throttle->control_in == 0 &&
        flight_stage != AP_SpdHgtControl::FLIGHT_TAKEOFF &&
        flight_stage != AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
        // manual rudder control while still
        steer_state.locked_course = false;
        steer_state.locked_course_err = 0;
        steering_control.steering = rudder_input;
        return;
    }

    float steer_rate = (rudder_input/4500.0f) * g.ground_steer_dps;
    if (flight_stage == AP_SpdHgtControl::FLIGHT_TAKEOFF ||
        flight_stage == AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
        steer_rate = 0;
    }
    if (!is_zero(steer_rate)) {
        // pilot is giving rudder input
        steer_state.locked_course = false;        
    } else if (!steer_state.locked_course) {
        // pilot has released the rudder stick or we are still - lock the course
        steer_state.locked_course = true;
        if (flight_stage != AP_SpdHgtControl::FLIGHT_TAKEOFF &&
            flight_stage != AP_SpdHgtControl::FLIGHT_LAND_ABORT) {
            steer_state.locked_course_err = 0;
        }
    }
    if (!steer_state.locked_course) {
        // use a rate controller at the pilot specified rate
        steering_control.steering = steerController.get_steering_out_rate(steer_rate);
    } else {
        // use a error controller on the summed error
        int32_t yaw_error_cd = -ToDeg(steer_state.locked_course_err)*100;
        steering_control.steering = steerController.get_steering_out_angle_error(yaw_error_cd);
    }
    steering_control.steering = constrain_int16(steering_control.steering, -4500, 4500);
}


/*
  calculate a new nav_pitch_cd from the speed height controller
 */
void Plane::calc_nav_pitch()
{
    // Calculate the Pitch of the plane
    // --------------------------------
    nav_pitch_cd = SpdHgt_Controller->get_pitch_demand();
    nav_pitch_cd = constrain_int32(nav_pitch_cd, pitch_limit_min_cd, aparm.pitch_limit_max_cd.get());
}

void Plane::calc_juland_nav_pitch()
{   
    jtnow= AP_HAL::millis();
    jdt = jtnow - jlast_t;
    if (jlast_t == 0 || jdt > 1000) {
    jdt = 0;
    }
    jlast_t = jtnow;  
    jdelta_time = (float)jdt * 0.001f;

    Vector3f posned;

    if (ahrs.get_relative_position_NED(posned)) {
        height_from_home = -posned.z;

        if (rangefinder.has_data() && 
        	((-posned.z)<=(rangefinder.max_distance_cm()*0.01f)) &&
             (rangefinder.status() != RangeFinder::RangeFinder_OutOfRangeHigh) &&
              (rangefinder.status() != RangeFinder::RangeFinder_OutOfRangeLow) ) {
                      height_from_home = rangefinder.distance_cm()*0.01f ;
            }
    }
    else {
        height_from_home = ahrs.get_baro().get_altitude();
        if ( rangefinder.has_data()   &&
             (ahrs.get_baro().get_altitude()<=(rangefinder.max_distance_cm()*0.01f))&&
             (rangefinder.status() != RangeFinder::RangeFinder_OutOfRangeHigh) &&
              (rangefinder.status() != RangeFinder::RangeFinder_OutOfRangeLow) ) {
                      height_from_home = rangefinder.distance_cm()*0.01f ;
            }
    }

	float sink_rate;
    Vector3f vel;
    if(ahrs.get_velocity_NED(vel)) {
        sink_rate = vel.z;
    } else if (gps.status() >= AP_GPS::GPS_OK_FIX_3D && gps.have_vertical_velocity()) {
        sink_rate = gps.velocity().z;
    } else {
       // sink_rate = -barometer.get_climb_rate();        
    
    
        /*
          use a complimentary filter to calculate climb_rate. This is
          designed to minimise lag
         */
        float baro_alt = ahrs.get_baro().get_altitude();
        // Get height acceleration
        float hgt_ddot_mea = -(ahrs.get_accel_ef().z + GRAVITY_MSS);
        // Perform filter calculation using backwards Euler integration
        // Coefficients selected to place all three filter poles at omega
        float _hgtCompFiltOmega = 3;
        float omega2 = _hgtCompFiltOmega*_hgtCompFiltOmega;
        float hgt_err = baro_alt - _height_filter_height;
        float integ1_input = hgt_err * omega2 * _hgtCompFiltOmega;

        _height_filter_dd_height += integ1_input * jdelta_time ;

        float integ2_input = _height_filter_dd_height + hgt_ddot_mea + hgt_err * omega2 * 3.0f;

        _jclimb_rate += integ2_input * jdelta_time ;

        sink_rate = - _jclimb_rate;

        float integ3_input = _jclimb_rate + hgt_err * _hgtCompFiltOmega * 3.0f;
        // If more than 1 second has elapsed since last update then reset the integrator state
        // to the measured height
        if (jdelta_time > 1.0f) {
            _height_filter_height = height_from_home;
        } else {
            _height_filter_height += integ3_input*jdelta_time ;
        }
    }
        /*
          when come into this mode ,command change gently
         */
        if(jinit_counter == 0) {
            jclimbrate_temp1 = -sink_rate;
            jtheta_init = ahrs.pitch_sensor - g.pitch_trim_cd - channel_throttle->servo_out * g.kff_throttle_to_pitch; 
            jtheta0 = jtheta_init;
            JU_climb_rate_err = 0;  
            climb_pid_info_I = 0;
            nav_pitch_cd_old = ahrs.pitch_sensor;
            //channel_throttle->servo_out = 30.0;
        }
        if(jinit_counter <= g.JU_init_transtime) {
            JU_climb_rate_err = jclimbrate_temp1 +  (g.JU_climbrate1 - jclimbrate_temp1) *  jinit_counter /(g.JU_init_transtime) - (-sink_rate);
            //channel_throttle->servo_out = 30.0 + 1.0 * jflare_counter;
            jtheta0 = jtheta_init + (g.JU_theta01*100.0f - jtheta_init) *  jinit_counter /(g.JU_init_transtime);
            jinit_counter += jdelta_time;
            }
        else {
            JU_climb_rate_err = g.JU_climbrate1 - (-sink_rate); 
            jtheta0 = g.JU_theta01*100.0f;
            //channel_throttle->servo_out = 34;  
             }
        /*
          when come into flare stage,just stimulate it once
         */
         if (height_from_home <= g.JU_flare_alt) {
            ju_flarestage = 1;
         }
        /*
          flare transition period
        */ 
         if (ju_flarestage == 1) {
               if(jflare_counter == 0) {
               jclimbrate_temp = -sink_rate;
               jtheta_init = ahrs.pitch_sensor - g.pitch_trim_cd - channel_throttle->servo_out * g.kff_throttle_to_pitch;
               jtheta0 = jtheta_init;
               JU_climb_rate_err = 0;  //change desend rate commad as flare alt's descend rate
               climb_pid_info_I = 0;      //clear integrater
               nav_pitch_cd_old = ahrs.pitch_sensor;
               }
               if(jflare_counter <= g.JU_flare_transition_time) {
               JU_climb_rate_err = jclimbrate_temp +  (g.JU_climbrate2 - jclimbrate_temp) *  jflare_counter /(g.JU_flare_transition_time) - (-sink_rate);
               jtheta0 = jtheta_init + (g.JU_theta02*100.0f - jtheta_init) *  jflare_counter /(g.JU_flare_transition_time);
               jflare_counter += jdelta_time;
               }
               else {
               JU_climb_rate_err = g.JU_climbrate2 - (-sink_rate);
               jtheta0 = g.JU_theta02*100.0f;  
               }  
         }
         else {
            jflare_counter = 0;
         }


        if (jdt>0) {
        climb_integrator_delta = JU_climb_rate_err * jdelta_time * g.JU_Iclimbrate * 5729.0f;    //5729 means rad to centidegree       
          if(nav_pitch_cd>g.JU_thetaoutmax*100.0f) {
          climb_integrator_delta = MIN(climb_integrator_delta,0);    
          } else if (nav_pitch_cd<-g.JU_thetaoutmax*100.0f) {
          climb_integrator_delta = MAX(climb_integrator_delta,0);
          }
          climb_pid_info_I += climb_integrator_delta;
          }
        else {
           climb_pid_info_I = 0;
        }
        climb_pid_info_I = constrain_float(climb_pid_info_I, -g.JU_Ioutmax*100.0f, g.JU_Ioutmax*100.0f);

        //channel_throttle->servo_out = 30.0;
        climbpout = JU_climb_rate_err * g.JU_Pclimbrate * 5729.0f ; //P's centidegree
        climbiout = climb_pid_info_I ;
        nav_pitch_cd = climbpout + climbiout + jtheta0;
        if (ju_flarestage == 1) {
            if (g.JU_flare_theta_enable == 1) {
                nav_pitch_cd = jtheta0;
            }
        }
        nav_pitch_cd = 0.3f * nav_pitch_cd + 0.7f * nav_pitch_cd_old;  //Apply first order lag 
        nav_pitch_cd = constrain_float(nav_pitch_cd, -g.JU_thetaoutmax*100.0f, g.JU_thetaoutmax*100.0f);
        nav_pitch_cd_old = nav_pitch_cd; 
}

void Plane::calc_juland_throttle()
{
    float EAS2TAS = ahrs.get_EAS2TAS();
    float EAS_dem = g.JU_speed1;
    jTAS_dem  = EAS_dem * EAS2TAS;
    if (!ahrs.airspeed_sensor_enabled() || !ahrs.airspeed_estimate(&jEAS)) {
        // If no airspeed available use average of min and max
        jEAS = 0.5f * (aparm.airspeed_min.get() + (float)aparm.airspeed_max.get());
    }
    float jTAS = jEAS * EAS2TAS;
    jTAS_err = jTAS_dem - jTAS;
    jTAS_err = constrain_float(jTAS_err, -g.JU_tho_Verr, g.JU_tho_Verr);
    JU_tho_pout = g.JU_tho_P * jTAS_err;

    int32_t last_throttle_servo_out = channel_throttle->servo_out;

        if(jinit_counter == 0) {
          throttle_servo_out_init1 = last_throttle_servo_out ;
          channel_throttle->servo_out = throttle_servo_out_init1;
        }
        if(jinit_counter <= g.JU_init_transtime) {
              channel_throttle->servo_out = (1-jinit_counter/g.JU_init_transtime) * throttle_servo_out_init1 + g.JU_tho_10 * (jinit_counter/g.JU_init_transtime) + JU_tho_pout * (jinit_counter/g.JU_init_transtime);
              //jinit_counter += jdelta_time; needn't plus counter again,already added in calc_juland_nav_pitch
            }
        else {
              channel_throttle->servo_out = g.JU_tho_10 + JU_tho_pout;
             }



        if (ju_flarestage == 1) {
               if(jthoflare_counter == 0) {
               throttle_servo_out_init2 = channel_throttle->servo_out;
               channel_throttle->servo_out = throttle_servo_out_init2;
               }
               if(jthoflare_counter <= g.JU_tho_flaret) {
               channel_throttle->servo_out = (1-jthoflare_counter/g.JU_tho_flaret) * throttle_servo_out_init2 +  g.JU_tho_20 * (jthoflare_counter/g.JU_tho_flaret);
               jthoflare_counter += jdelta_time;
               }
               else {
               channel_throttle->servo_out = g.JU_tho_20;
               }  
         }
         else {
            jthoflare_counter = 0;
         }

         channel_throttle->servo_out = constrain_int32(channel_throttle->servo_out, 0, 100);
}

/*
  calculate a new nav_roll_cd from the navigation controller
 */
void Plane::calc_nav_roll()
{
    nav_roll_cd = nav_controller->nav_roll_cd();
    update_load_factor();
    nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit_cd, roll_limit_cd);
}

void Plane::calc_juland_nav_roll()
{
   float bearingtrue = ahrs.yaw_sensor/100.0f;
   //AP_Mission::Mission_Command cmd;

if (g.Jinityawable == 1) {
   JU_bearing_cmd =  g.JU_phsi_0 + channel_rudder->pwm_to_angle()/100.0f;//degree channel_rudder->pwm_to_angle() is a value from -4500 ~4500
   } // only control phsi. 


else {
 mission.set_current_cmd(2);
 ju_next_WP = mission.get_current_nav_cmd().content.location;
 mission.set_current_cmd(1);
 ju_prev_WP = mission.get_current_nav_cmd().content.location;


/*ju_next_WP.lat = (int32_t)(-35.350977 * 1.0e7f);
ju_next_WP.lng = (int32_t)(149.163869 * 1.0e7f);


ju_prev_WP.lat = (int32_t)(-35.366436 * 1.0e7f);
ju_prev_WP.lng = (int32_t)(149.165482 * 1.0e7f);*/




 jS1 = get_distance(current_loc,ju_next_WP);
 jbearing1 = get_bearing_cd(current_loc,ju_next_WP)*0.01f; 
 jbearing2 = get_bearing_cd(ju_prev_WP,ju_next_WP)*0.01f; 
 float jbearing_err = jbearing1 -jbearing2;

 if (jbearing_err<-180.0f) {
     jbearing_err = 360.0f + jbearing_err;
     }  
 if (jbearing_err>180.0f) {
     jbearing_err = jbearing_err - 360.0f;
     }  

 jdeltay_err = jS1 * sinf(jbearing_err/57.3f) + channel_rudder->pwm_to_angle()/300.0f;



 JU_bearing_cmd = jdeltay_err * g.JU_y_P;



 JU_bearing_cmd = constrain_float(JU_bearing_cmd, -90.0f, 90.0f);
 JU_bearing_cmd +=g.JU_phsi_0;

/* plane.gcs_send_text_fmt(MAV_SEVERITY_INFO, "jdeltay_er = %.4fm",
                                        (float)(jdeltay_err));*/
 
}


 //phsi command is given by delta y error.


   if (JU_bearing_cmd>360.0f) {
   	JU_bearing_cmd = JU_bearing_cmd - 360.0f;
   }
   if (JU_bearing_cmd<0.0f) {
   	JU_bearing_cmd = 360.0f + JU_bearing_cmd;
   }

   JU_bearing_cmd = constrain_float(JU_bearing_cmd ,0,360.0f);

   float bearing_err = JU_bearing_cmd - bearingtrue;
   if (bearing_err<-180.0f) {
     bearing_err = 360.0f + bearing_err;
    }  
   if (bearing_err>180.0f) {
     bearing_err = bearing_err - 360.0f;
    }  

   nav_roll_cd = bearing_err * g.JU_phsi_P *100.0f;

     if (ju_flarestage == 1) {
     nav_roll_cd = constrain_int32(nav_roll_cd, -1000, 1000);
       if  (height_from_home<=1.0f) {
          nav_roll_cd = 0 ;
        }

   }
}
/*****************************************
* Throttle slew limit
*****************************************/
void Plane::throttle_slew_limit(int16_t last_throttle)
{
    uint8_t slewrate = aparm.throttle_slewrate;
    if (control_mode==AUTO && auto_state.takeoff_complete == false && g.takeoff_throttle_slewrate != 0) {
        slewrate = g.takeoff_throttle_slewrate;
    }
    // if slew limit rate is set to zero then do not slew limit
    if (slewrate) {                   
        // limit throttle change by the given percentage per second
        float temp = slewrate * G_Dt * 0.01f * fabsf(channel_throttle->radio_max - channel_throttle->radio_min);
        // allow a minimum change of 1 PWM per cycle
        if (temp < 1) {
            temp = 1;
        }
        channel_throttle->radio_out = constrain_int16(channel_throttle->radio_out, last_throttle - temp, last_throttle + temp);
    }
}

/*****************************************
Flap slew limit
*****************************************/
void Plane::flap_slew_limit(int8_t &last_value, int8_t &new_value)
{
    uint8_t slewrate = g.flap_slewrate;
    // if slew limit rate is set to zero then do not slew limit
    if (slewrate) {                   
        // limit flap change by the given percentage per second
        float temp = slewrate * G_Dt;
        // allow a minimum change of 1% per cycle. This means the
        // slowest flaps we can do is full change over 2 seconds
        if (temp < 1) {
            temp = 1;
        }
        new_value = constrain_int16(new_value, last_value - temp, last_value + temp);
    }
    last_value = new_value;
}

/* We want to suppress the throttle if we think we are on the ground and in an autopilot controlled throttle mode.

   Disable throttle if following conditions are met:
   *       1 - We are in Circle mode (which we use for short term failsafe), or in FBW-B or higher
   *       AND
   *       2 - Our reported altitude is within 10 meters of the home altitude.
   *       3 - Our reported speed is under 5 meters per second.
   *       4 - We are not performing a takeoff in Auto mode or takeoff speed/accel not yet reached
   *       OR
   *       5 - Home location is not set
*/
bool Plane::suppress_throttle(void)
{
    if (auto_throttle_mode && parachute.released()) {
        // throttle always suppressed in auto-throttle modes after parachute release
        throttle_suppressed = true;
        return true;
    }
    
    if (!throttle_suppressed) {
        // we've previously met a condition for unsupressing the throttle
        return false;
    }
    if (!auto_throttle_mode) {
        // the user controls the throttle
        throttle_suppressed = false;
        return false;
    }

    if (control_mode==AUTO && g.auto_fbw_steer) {
        // user has throttle control
        return false;
    }

    bool gps_movement = (gps.status() >= AP_GPS::GPS_OK_FIX_2D && gps.ground_speed() >= 5);
    
    if (control_mode==AUTO && 
        auto_state.takeoff_complete == false) {

        uint32_t launch_duration_ms = ((int32_t)g.takeoff_throttle_delay)*100 + 2000;
        if (is_flying() &&
            millis() - started_flying_ms > MAX(launch_duration_ms, 5000U) && // been flying >5s in any mode
            adjusted_relative_altitude_cm() > 500 && // are >5m above AGL/home
            labs(ahrs.pitch_sensor) < 3000 && // not high pitch, which happens when held before launch
            gps_movement) { // definate gps movement
            // we're already flying, do not suppress the throttle. We can get
            // stuck in this condition if we reset a mission and cmd 1 is takeoff
            // but we're currently flying around below the takeoff altitude
            throttle_suppressed = false;
            return false;
        }
        if (auto_takeoff_check()) {
            // we're in auto takeoff 
            throttle_suppressed = false;
            auto_state.baro_takeoff_alt = barometer.get_altitude();
            return false;
        }
        // keep throttle suppressed
        return true;
    }
    
    if (relative_altitude_abs_cm() >= 1000) {
        // we're more than 10m from the home altitude
        throttle_suppressed = false;
        gcs_send_text_fmt(MAV_SEVERITY_INFO, "Throttle enabled. Altitude %.2f",
                          (double)(relative_altitude_abs_cm()*0.01f));
        return false;
    }

    if (gps_movement) {
        // if we have an airspeed sensor, then check it too, and
        // require 5m/s. This prevents throttle up due to spiky GPS
        // groundspeed with bad GPS reception
        if ((!ahrs.airspeed_sensor_enabled()) || airspeed.get_airspeed() >= 5) {
            // we're moving at more than 5 m/s
            gcs_send_text_fmt(MAV_SEVERITY_INFO, "Throttle enabled. Speed %.2f airspeed %.2f",
                              (double)gps.ground_speed(),
                              (double)airspeed.get_airspeed());
            throttle_suppressed = false;
            return false;        
        }
    }

    if (quadplane.is_flying()) {
        gcs_send_text_fmt(MAV_SEVERITY_INFO, "Throttle enabled VTOL");
        throttle_suppressed = false;
    }

    // throttle remains suppressed
    return true;
}

/*
  implement a software VTail or elevon mixer. There are 4 different mixing modes
 */
void Plane::channel_output_mixer(uint8_t mixing_type, int16_t &chan1_out, int16_t &chan2_out)
{
    int16_t c1, c2;
    int16_t v1, v2;

    // first get desired elevator and rudder as -500..500 values
    c1 = chan1_out - channel_pitch->radio_trim;
    c2 = chan2_out - channel_roll->radio_trim;

    v1 = (c1 - c2) * g.mixing_gain;
    v2 = (c1 + c2) * g.mixing_gain;

    // now map to mixed output
    switch (mixing_type) {
    case MIXING_DISABLED:
        return;

    case MIXING_UPUP:
        break;

    case MIXING_UPDN:
        v2 = -v2;
        break;

    case MIXING_DNUP:
        v1 = -v1;
        break;

    case MIXING_DNDN:
        v1 = -v1;
        v2 = -v2;
        break;
    }

    // scale for a 1500 center and 900..2100 range, symmetric
    v1 = constrain_int16(v1, -600, 600);
    v2 = constrain_int16(v2, -600, 600);

    chan1_out = 1500 + v1;
    chan2_out = 1500 + v2;
}

/*
  setup flaperon output channels
 */
void Plane::flaperon_update(int8_t flap_percent)
{
    if (!RC_Channel_aux::function_assigned(RC_Channel_aux::k_flaperon1) ||
        !RC_Channel_aux::function_assigned(RC_Channel_aux::k_flaperon2)) {
        return;
    }
    int16_t ch1, ch2;
    /*
      flaperons are implemented as a mixer between aileron and a
      percentage of flaps. Flap input can come from a manual channel
      or from auto flaps.

      Use k_flaperon1 and k_flaperon2 channel trims to center servos.
      Then adjust aileron trim for level flight (note that aileron trim is affected
      by mixing gain). flapin_channel's trim is not used.
     */
     
    ch1 = channel_roll->radio_out;
    // The *5 is to take a percentage to a value from -500 to 500 for the mixer
    ch2 = 1500 - flap_percent * 5;
    channel_output_mixer(g.flaperon_output, ch1, ch2);
    RC_Channel_aux::set_radio_trimmed(RC_Channel_aux::k_flaperon1, ch1);
    RC_Channel_aux::set_radio_trimmed(RC_Channel_aux::k_flaperon2, ch2);
}

/*
  setup servos for idle mode
  Idle mode is used during balloon launch to keep servos still, apart
  from occasional wiggle to prevent freezing up
 */
void Plane::set_servos_idle(void)
{
    RC_Channel_aux::output_ch_all();
    if (auto_state.idle_wiggle_stage == 0) {
        RC_Channel::output_trim_all();
        return;
    }
    int16_t servo_value = 0;
    // move over full range for 2 seconds
    auto_state.idle_wiggle_stage += 2;
    if (auto_state.idle_wiggle_stage < 50) {
        servo_value = auto_state.idle_wiggle_stage * (4500 / 50);
    } else if (auto_state.idle_wiggle_stage < 100) {
        servo_value = (100 - auto_state.idle_wiggle_stage) * (4500 / 50);        
    } else if (auto_state.idle_wiggle_stage < 150) {
        servo_value = (100 - auto_state.idle_wiggle_stage) * (4500 / 50);        
    } else if (auto_state.idle_wiggle_stage < 200) {
        servo_value = (auto_state.idle_wiggle_stage-200) * (4500 / 50);        
    } else {
        auto_state.idle_wiggle_stage = 0;
    }
    channel_roll->servo_out = servo_value;
    channel_pitch->servo_out = servo_value;
    channel_rudder->servo_out = servo_value;
    channel_roll->calc_pwm();
    channel_pitch->calc_pwm();
    channel_rudder->calc_pwm();
    channel_roll->output();
    channel_pitch->output();
    channel_throttle->output();
    channel_rudder->output();
    channel_throttle->output_trim();
}

/*
  return minimum throttle, taking account of throttle reversal
 */
uint16_t Plane::throttle_min(void) const
{
    return channel_throttle->get_reverse() ? channel_throttle->radio_max : channel_throttle->radio_min;
};


/*****************************************
* Set the flight control servos based on the current calculated values
*****************************************/
void Plane::set_servos(void)
{
    int16_t last_throttle = channel_throttle->radio_out;

    // do any transition updates for quadplane
    quadplane.update();    

    if (control_mode == AUTO && auto_state.idle_mode) {
        // special handling for balloon launch
        set_servos_idle();
        return;
    }

    /*
      see if we are doing ground steering.
     */
    if (!steering_control.ground_steering) {
        // we are not at an altitude for ground steering. Set the nose
        // wheel to the rudder just in case the barometer has drifted
        // a lot
        steering_control.steering = steering_control.rudder;
    } else if (!RC_Channel_aux::function_assigned(RC_Channel_aux::k_steering)) {
        // we are within the ground steering altitude but don't have a
        // dedicated steering channel. Set the rudder to the ground
        // steering output
        steering_control.rudder = steering_control.steering;
    }
    channel_rudder->servo_out = steering_control.rudder;

    // clear ground_steering to ensure manual control if the yaw stabilizer doesn't run
    steering_control.ground_steering = false;

    RC_Channel_aux::set_servo_out(RC_Channel_aux::k_rudder, steering_control.rudder);
    RC_Channel_aux::set_servo_out(RC_Channel_aux::k_steering, steering_control.steering);

    if (control_mode == MANUAL) {
        // do a direct pass through of radio values
        if (g.mix_mode == 0 || g.elevon_output != MIXING_DISABLED) {
            channel_roll->radio_out                = channel_roll->radio_in;
            channel_pitch->radio_out               = channel_pitch->radio_in;
        } else {
            channel_roll->radio_out                = channel_roll->read();
            channel_pitch->radio_out               = channel_pitch->read();
        }
        channel_throttle->radio_out    = channel_throttle->radio_in;
        channel_rudder->radio_out              = channel_rudder->radio_in;

        // setup extra channels. We want this to come from the
        // main input channel, but using the 2nd channels dead
        // zone, reverse and min/max settings. We need to use
        // pwm_to_angle_dz() to ensure we don't trim the value for the
        // deadzone of the main aileron channel, otherwise the 2nd
        // aileron won't quite follow the first one
        RC_Channel_aux::set_servo_out(RC_Channel_aux::k_aileron, channel_roll->pwm_to_angle_dz(0));
        RC_Channel_aux::set_servo_out(RC_Channel_aux::k_elevator, channel_pitch->pwm_to_angle_dz(0));

        // this variant assumes you have the corresponding
        // input channel setup in your transmitter for manual control
        // of the 2nd aileron
        RC_Channel_aux::copy_radio_in_out(RC_Channel_aux::k_aileron_with_input);
        RC_Channel_aux::copy_radio_in_out(RC_Channel_aux::k_elevator_with_input);

        if (g.mix_mode == 0 && g.elevon_output == MIXING_DISABLED) {
            // set any differential spoilers to follow the elevons in
            // manual mode. 
            RC_Channel_aux::set_radio(RC_Channel_aux::k_dspoiler1, channel_roll->radio_out);
            RC_Channel_aux::set_radio(RC_Channel_aux::k_dspoiler2, channel_pitch->radio_out);
        }
    } else {
        if (g.mix_mode == 0) {
            // both types of secondary aileron are slaved to the roll servo out
            RC_Channel_aux::set_servo_out(RC_Channel_aux::k_aileron, channel_roll->servo_out);
            RC_Channel_aux::set_servo_out(RC_Channel_aux::k_aileron_with_input, channel_roll->servo_out);

            // both types of secondary elevator are slaved to the pitch servo out
            RC_Channel_aux::set_servo_out(RC_Channel_aux::k_elevator, channel_pitch->servo_out);
            RC_Channel_aux::set_servo_out(RC_Channel_aux::k_elevator_with_input, channel_pitch->servo_out);
        }else{
            /*Elevon mode*/
            float ch1;
            float ch2;
            ch1 = channel_pitch->servo_out - (BOOL_TO_SIGN(g.reverse_elevons) * channel_roll->servo_out);
            ch2 = channel_pitch->servo_out + (BOOL_TO_SIGN(g.reverse_elevons) * channel_roll->servo_out);

			/* Differential Spoilers
               If differential spoilers are setup, then we translate
               rudder control into splitting of the two ailerons on
               the side of the aircraft where we want to induce
               additional drag.
             */
			if (RC_Channel_aux::function_assigned(RC_Channel_aux::k_dspoiler1) && RC_Channel_aux::function_assigned(RC_Channel_aux::k_dspoiler2)) {
				float ch3 = ch1;
				float ch4 = ch2;
				if ( BOOL_TO_SIGN(g.reverse_elevons) * channel_rudder->servo_out < 0) {
				    ch1 += abs(channel_rudder->servo_out);
				    ch3 -= abs(channel_rudder->servo_out);
				} else {
					ch2 += abs(channel_rudder->servo_out);
				    ch4 -= abs(channel_rudder->servo_out);
				}
				RC_Channel_aux::set_servo_out(RC_Channel_aux::k_dspoiler1, ch3);
				RC_Channel_aux::set_servo_out(RC_Channel_aux::k_dspoiler2, ch4);
			}

            // directly set the radio_out values for elevon mode
            channel_roll->radio_out  =     elevon.trim1 + (BOOL_TO_SIGN(g.reverse_ch1_elevon) * (ch1 * 500.0f/ SERVO_MAX));
            channel_pitch->radio_out =     elevon.trim2 + (BOOL_TO_SIGN(g.reverse_ch2_elevon) * (ch2 * 500.0f/ SERVO_MAX));
        }

        // push out the PWM values
        if (g.mix_mode == 0) {
            channel_roll->calc_pwm();
            channel_pitch->calc_pwm();
        }
        channel_rudder->calc_pwm();

#if THROTTLE_OUT == 0
        channel_throttle->servo_out = 0;
#else
        // convert 0 to 100% into PWM
        uint8_t min_throttle = aparm.throttle_min.get();
        uint8_t max_throttle = aparm.throttle_max.get();
        if (control_mode == AUTO && flight_stage == AP_SpdHgtControl::FLIGHT_LAND_FINAL) {
            min_throttle = 0;
        }
        if (control_mode == AUTO &&
            (flight_stage == AP_SpdHgtControl::FLIGHT_TAKEOFF || flight_stage == AP_SpdHgtControl::FLIGHT_LAND_ABORT)) {
            if(aparm.takeoff_throttle_max != 0) {
                max_throttle = aparm.takeoff_throttle_max;
            } else {
                max_throttle = aparm.throttle_max;
            }
        }
        channel_throttle->servo_out = constrain_int16(channel_throttle->servo_out, 
                                                      min_throttle,
                                                      max_throttle);

        if (!hal.util->get_soft_armed()) {
            channel_throttle->servo_out = 0;
            channel_throttle->calc_pwm();                
        } else if (suppress_throttle()) {
            // throttle is suppressed in auto mode
            channel_throttle->servo_out = 0;
            if (g.throttle_suppress_manual) {
                // manual pass through of throttle while throttle is suppressed
                channel_throttle->radio_out = channel_throttle->radio_in;
            } else {
                channel_throttle->calc_pwm();                
            }
        } else if (g.throttle_passthru_stabilize && 
                   (control_mode == STABILIZE || 
                    control_mode == TRAINING ||
                    control_mode == ACRO ||
                    control_mode == FLY_BY_WIRE_A ||
                    control_mode == AUTOTUNE ||
                    control_mode == JULAND)) {
            // manual pass through of throttle while in FBWA or
            // STABILIZE mode with THR_PASS_STAB set
            channel_throttle->radio_out = channel_throttle->radio_in;
        } else if (control_mode == GUIDED && 
                   guided_throttle_passthru) {
            // manual pass through of throttle while in GUIDED
            channel_throttle->radio_out = channel_throttle->radio_in;
        } else if (control_mode == QSTABILIZE ||
                   control_mode == QHOVER ||
                   control_mode == QLOITER ||
                   quadplane.in_vtol_auto()) {
            // no forward throttle for now
            channel_throttle->servo_out = 0;
            channel_throttle->calc_pwm();
        } else {
            // normal throttle calculation based on servo_out
            channel_throttle->calc_pwm();
        }
#endif
    }

    // Auto flap deployment
    int8_t auto_flap_percent = 0;
    int8_t manual_flap_percent = 0;
    static int8_t last_auto_flap;
    static int8_t last_manual_flap;

    // work out any manual flap input
    RC_Channel *flapin = RC_Channel::rc_channel(g.flapin_channel-1);
    if (flapin != NULL && !failsafe.ch3_failsafe && failsafe.ch3_counter == 0) {
        flapin->input();
        manual_flap_percent = flapin->percent_input();
    }

    if (auto_throttle_mode) {
        int16_t flapSpeedSource = 0;
        if (ahrs.airspeed_sensor_enabled()) {
            flapSpeedSource = target_airspeed_cm * 0.01f;
        } else {
            flapSpeedSource = aparm.throttle_cruise;
        }
        if (g.flap_2_speed != 0 && flapSpeedSource <= g.flap_2_speed) {
            auto_flap_percent = g.flap_2_percent;
        } else if ( g.flap_1_speed != 0 && flapSpeedSource <= g.flap_1_speed) {
            auto_flap_percent = g.flap_1_percent;
        } //else flaps stay at default zero deflection

        /*
          special flap levels for takeoff and landing. This works
          better than speed based flaps as it leads to less
          possibility of oscillation
         */
        if (control_mode == AUTO) {
            switch (flight_stage) {
            case AP_SpdHgtControl::FLIGHT_TAKEOFF:
            case AP_SpdHgtControl::FLIGHT_LAND_ABORT:
                if (g.takeoff_flap_percent != 0) {
                    auto_flap_percent = g.takeoff_flap_percent;
                }
                break;
            case AP_SpdHgtControl::FLIGHT_LAND_APPROACH:
            case AP_SpdHgtControl::FLIGHT_LAND_FINAL:
                if (g.land_flap_percent != 0) {
                    auto_flap_percent = g.land_flap_percent;
                }
                break;
            default:
                break;
            }
        }
    }

    // manual flap input overrides auto flap input
    if (abs(manual_flap_percent) > auto_flap_percent) {
        auto_flap_percent = manual_flap_percent;
    }

    flap_slew_limit(last_auto_flap, auto_flap_percent);
    flap_slew_limit(last_manual_flap, manual_flap_percent);

    RC_Channel_aux::set_servo_out(RC_Channel_aux::k_flap_auto, auto_flap_percent);
    RC_Channel_aux::set_servo_out(RC_Channel_aux::k_flap, manual_flap_percent);

    if (control_mode >= FLY_BY_WIRE_B) {
        /* only do throttle slew limiting in modes where throttle
         *  control is automatic */
        throttle_slew_limit(last_throttle);
    }

    if (control_mode == TRAINING) {
        // copy rudder in training mode
        channel_rudder->radio_out   = channel_rudder->radio_in;
    }

    if (g.flaperon_output != MIXING_DISABLED && g.elevon_output == MIXING_DISABLED && g.mix_mode == 0) {
        flaperon_update(auto_flap_percent);
    }
    if (g.vtail_output != MIXING_DISABLED) {
        channel_output_mixer(g.vtail_output, channel_pitch->radio_out, channel_rudder->radio_out);
    } else if (g.elevon_output != MIXING_DISABLED) {
        channel_output_mixer(g.elevon_output, channel_pitch->radio_out, channel_roll->radio_out);
    }

    if (!arming.is_armed()) {
        //Some ESCs get noisy (beep error msgs) if PWM == 0.
        //This little segment aims to avoid this.
        switch (arming.arming_required()) { 
        case AP_Arming::NO:
            //keep existing behavior: do nothing to radio_out
            //(don't disarm throttle channel even if AP_Arming class is)
            break;

        case AP_Arming::YES_ZERO_PWM:
            channel_throttle->radio_out = 0;
            break;

        case AP_Arming::YES_MIN_PWM:
        default:
            channel_throttle->radio_out = throttle_min();
            break;
        }
    }

#if OBC_FAILSAFE == ENABLED
    // this is to allow the failsafe module to deliberately crash 
    // the plane. Only used in extreme circumstances to meet the
    // OBC rules
    obc.check_crash_plane();
#endif

#if HIL_SUPPORT
    if (g.hil_mode == 1) {
        // get the servos to the GCS immediately for HIL
        if (comm_get_txspace(MAVLINK_COMM_0) >= 
            MAVLINK_MSG_ID_RC_CHANNELS_SCALED_LEN + MAVLINK_NUM_NON_PAYLOAD_BYTES) {
            send_servo_out(MAVLINK_COMM_0);
        }
        if (!g.hil_servos) {
            return;
        }
    }
#endif

    // send values to the PWM timers for output
    // ----------------------------------------
    if (g.rudder_only == 0) {
        // when we RUDDER_ONLY mode we don't send the channel_roll
        // output and instead rely on KFF_RDDRMIX. That allows the yaw
        // damper to operate.
        channel_roll->output();
    }
    channel_pitch->output();
    channel_throttle->output();
    channel_rudder->output();
    RC_Channel_aux::output_ch_all();
}

void Plane::demo_servos(uint8_t i) 
{
    while(i > 0) {
        gcs_send_text(MAV_SEVERITY_INFO,"Demo servos");
        demoing_servos = true;
        servo_write(1, 1400);
        hal.scheduler->delay(400);
        servo_write(1, 1600);
        hal.scheduler->delay(200);
        servo_write(1, 1500);
        demoing_servos = false;
        hal.scheduler->delay(400);
        i--;
    }
}

/*
  adjust nav_pitch_cd for STAB_PITCH_DOWN_CD. This is used to make
  keeping up good airspeed in FBWA mode easier, as the plane will
  automatically pitch down a little when at low throttle. It makes
  FBWA landings without stalling much easier.
 */
void Plane::adjust_nav_pitch_throttle(void)
{
    uint8_t throttle = throttle_percentage();
    if (throttle < aparm.throttle_cruise && flight_stage != AP_SpdHgtControl::FLIGHT_VTOL) {
        float p = (aparm.throttle_cruise - throttle) / (float)aparm.throttle_cruise;
        nav_pitch_cd -= g.stab_pitch_down * 100.0f * p;
    }
}


/*
  calculate a new aerodynamic_load_factor and limit nav_roll_cd to
  ensure that the load factor does not take us below the sustainable
  airspeed
 */
void Plane::update_load_factor(void)
{
    float demanded_roll = fabsf(nav_roll_cd*0.01f);
    if (demanded_roll > 85) {
        // limit to 85 degrees to prevent numerical errors
        demanded_roll = 85;
    }
    aerodynamic_load_factor = 1.0f / safe_sqrt(cosf(radians(demanded_roll)));

    if (!aparm.stall_prevention) {
        // stall prevention is disabled
        return;
    }
    if (fly_inverted()) {
        // no roll limits when inverted
        return;
    }

    float max_load_factor = smoothed_airspeed / aparm.airspeed_min;
    if (max_load_factor <= 1) {
        // our airspeed is below the minimum airspeed. Limit roll to
        // 25 degrees
        nav_roll_cd = constrain_int32(nav_roll_cd, -2500, 2500);
        roll_limit_cd = constrain_int32(roll_limit_cd, -2500, 2500);
    } else if (max_load_factor < aerodynamic_load_factor) {
        // the demanded nav_roll would take us past the aerodymamic
        // load limit. Limit our roll to a bank angle that will keep
        // the load within what the airframe can handle. We always
        // allow at least 25 degrees of roll however, to ensure the
        // aircraft can be maneuvered with a bad airspeed estimate. At
        // 25 degrees the load factor is 1.1 (10%)
        int32_t roll_limit = degrees(acosf(sq(1.0f / max_load_factor)))*100;
        if (roll_limit < 2500) {
            roll_limit = 2500;
        }
        nav_roll_cd = constrain_int32(nav_roll_cd, -roll_limit, roll_limit);
        roll_limit_cd = constrain_int32(roll_limit_cd, -roll_limit, roll_limit);
    }    
}
