/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "link_queue.hh"
#include "timestamp.hh"
#include "util.hh"
#include "ezio.hh"
#include "abstract_packet_queue.hh"

using namespace std;

LinkQueue::LinkQueue( const string & link_name, const string & filename, const string & logfile,
                      const bool repeat, const bool graph_throughput, const bool graph_delay,
                      unique_ptr<AbstractPacketQueue> && packet_queue,
                      const string & command_line )
    : next_delivery_( 0 ),
      schedule_(),
      base_timestamp_( timestamp() ),
      packet_queue_( move( packet_queue ) ),
      cellular_queue_( nullptr ),
      use_cellular_queue_( link_name == "Uplink" ),
      packet_in_transit_( "", 0 ),
      packet_in_transit_bytes_left_( 0 ),
      output_queue_(),
      log_(),
      throughput_graph_( nullptr ),
      delay_graph_( nullptr ),
      repeat_( repeat ),
      finished_( false )
{
    assert_not_root();

    ifstream trace_file( filename );

    if ( not trace_file.good() ) {
        throw runtime_error( filename + ": error opening for reading" );
    }

    string line;

    while ( trace_file.good() and getline( trace_file, line ) ) {
        if ( line.empty() ) {
            throw runtime_error( filename + ": invalid empty line" );
        }

        const uint64_t ms = myatoi( line );

        if ( not schedule_.empty() and ms < schedule_.back() ) {
            throw runtime_error( filename + ": timestamps must be monotonically nondecreasing" );
        }

        schedule_.emplace_back( ms );
    }

    if ( schedule_.empty() ) {
        throw runtime_error( filename + ": no valid timestamps found" );
    }

    if ( schedule_.back() == 0 ) {
        throw runtime_error( filename + ": trace must last for a nonzero amount of time" );
    }

    if ( use_cellular_queue_ ) {
        cellular_emulation::TwoTierQueueConfig config;
        config.driver_queue_config.max_queue_size = 128;
        config.driver_queue_config.max_packet_size = PACKET_SIZE;
        config.driver_queue_config.signal_export_interval_ms = 1;
        config.driver_queue_config.signal_export_path = "/tmp/mm_virtual_driver_queue";
        config.modem_buffer_config.max_buffer_size = 100;
        config.signal_export_path = "/tmp/mm_virtual_driver_queue";
        config.enable_signal_export = true;
        config.enable_csv_logging = true;
        config.csv_log_path = "/tmp/mm_aether_monitor.csv";
        /* "multi" = push to BPF QUEUE map + append to CSV log for verification */
        config.signal_exporter_type = "multi";
        cellular_queue_.reset( new cellular_emulation::TwoTierQueueManager( config ) );
    }

    if ( not logfile.empty() ) {
        log_.reset( new ofstream( logfile ) );
        if ( not log_->good() ) {
            throw runtime_error( logfile + ": error opening for writing" );
        }

        *log_ << "# mahimahi mm-link (" << link_name << ") [" << filename << "] > " << logfile << endl;
        *log_ << "# command line: " << command_line << endl;
        *log_ << "# queue: "
              << ( use_cellular_queue_ ? cellular_queue_->to_string()
                                      : packet_queue_->to_string() )
              << endl;
        *log_ << "# init timestamp: " << initial_timestamp() << endl;
        *log_ << "# base timestamp: " << base_timestamp_ << endl;
        const char * prefix = getenv( "MAHIMAHI_SHELL_PREFIX" );
        if ( prefix ) {
            *log_ << "# mahimahi config: " << prefix << endl;
        }
    }

    if ( graph_throughput ) {
        throughput_graph_.reset( new BinnedLiveGraph( link_name + " [" + filename + "]",
                                                      { make_tuple( 1.0, 0.0, 0.0, 0.25, true ),
                                                        make_tuple( 0.0, 0.0, 0.4, 1.0, false ),
                                                        make_tuple( 1.0, 0.0, 0.0, 0.5, false ) },
                                                      "throughput (Mbps)",
                                                      8.0 / 1000000.0,
                                                      true,
                                                      500,
                                                      [] ( int, int & x ) { x = 0; } ) );
    }

    if ( graph_delay ) {
        delay_graph_.reset( new BinnedLiveGraph( link_name + " delay [" + filename + "]",
                                                 { make_tuple( 0.0, 0.25, 0.0, 1.0, false ) },
                                                 "queueing delay (ms)",
                                                 1, false, 250,
                                                 [] ( int, int & x ) { x = -1; } ) );
    }
}

void LinkQueue::record_arrival( const uint64_t arrival_time, const size_t pkt_size )
{
    if ( log_ ) {
        *log_ << arrival_time << " + " << pkt_size << endl;
    }

    if ( throughput_graph_ ) {
        throughput_graph_->add_value_now( 1, pkt_size );
    }
}

void LinkQueue::record_drop( const uint64_t time, const size_t pkts_dropped, const size_t bytes_dropped )
{
    if ( log_ ) {
        *log_ << time << " d " << pkts_dropped << " " << bytes_dropped << endl;
    }
}

void LinkQueue::record_departure_opportunity( void )
{
    if ( log_ ) {
        *log_ << next_delivery_time() << " # " << PACKET_SIZE << endl;
    }

    if ( throughput_graph_ ) {
        throughput_graph_->add_value_now( 0, PACKET_SIZE );
    }
}

void LinkQueue::record_departure( const uint64_t departure_time, const QueuedPacket & packet )
{
    if ( log_ ) {
        *log_ << departure_time << " - " << packet.contents.size()
              << " " << departure_time - packet.arrival_time << endl;
    }

    if ( throughput_graph_ ) {
        throughput_graph_->add_value_now( 2, packet.contents.size() );
    }

    if ( delay_graph_ ) {
        delay_graph_->set_max_value_now( 0, departure_time - packet.arrival_time );
    }
}

void LinkQueue::read_packet( const string & contents )
{
    const uint64_t now = timestamp();

    if ( contents.size() > PACKET_SIZE ) {
        throw runtime_error( "packet size is greater than maximum" );
    }

    rationalize( now );
    record_arrival( now, contents.size() );

    if ( use_cellular_queue_ ) {
        if ( not cellular_queue_->receive_packet( contents, now ) ) {
            record_drop( now, 1, contents.size() );
        }
        cellular_queue_->update( now );
    } else {
        packet_queue_->enqueue( QueuedPacket( contents, now ) );
    }
}

QueuedPacket LinkQueue::dequeue_for_transmission( const uint64_t current_time )
{
    if ( use_cellular_queue_ ) {
        cellular_queue_->update( current_time );
        return cellular_queue_->drain_packet( current_time );
    }

    if ( packet_queue_->empty() ) {
        return QueuedPacket( "", 0 );
    }

    return packet_queue_->dequeue();
}

uint64_t LinkQueue::next_delivery_time( void ) const
{
    if ( finished_ ) {
        return -1;
    } else {
        return schedule_.at( next_delivery_ ) + base_timestamp_;
    }
}

void LinkQueue::use_a_delivery_opportunity( void )
{
    record_departure_opportunity();

    next_delivery_ = ( next_delivery_ + 1 ) % schedule_.size();

    if ( next_delivery_ == 0 ) {
        if ( repeat_ ) {
            base_timestamp_ += schedule_.back();
        } else {
            finished_ = true;
        }
    }
}

void LinkQueue::rationalize( const uint64_t now )
{
    while ( next_delivery_time() <= now ) {
        const uint64_t this_delivery_time = next_delivery_time();

        unsigned int bytes_left_in_this_delivery = PACKET_SIZE;
        use_a_delivery_opportunity();

        while ( bytes_left_in_this_delivery > 0 ) {
            if ( not packet_in_transit_bytes_left_ ) {
                packet_in_transit_ = dequeue_for_transmission( this_delivery_time );
                if ( packet_in_transit_.contents.empty() ) {
                    break;
                }
                packet_in_transit_bytes_left_ = packet_in_transit_.contents.size();
            }

            assert( packet_in_transit_.arrival_time <= this_delivery_time );
            assert( packet_in_transit_bytes_left_ <= PACKET_SIZE );
            assert( packet_in_transit_bytes_left_ > 0 );
            assert( packet_in_transit_bytes_left_ <= packet_in_transit_.contents.size() );

            const unsigned int amount_to_send = min( bytes_left_in_this_delivery,
                                                     packet_in_transit_bytes_left_ );

            packet_in_transit_bytes_left_ -= amount_to_send;
            bytes_left_in_this_delivery -= amount_to_send;

            if ( packet_in_transit_bytes_left_ == 0 ) {
                record_departure( this_delivery_time, packet_in_transit_ );
                output_queue_.push( move( packet_in_transit_.contents ) );
            }
        }
    }

    if ( use_cellular_queue_ ) {
        cellular_queue_->update( now );
    }
}

void LinkQueue::write_packets( FileDescriptor & fd )
{
    while ( not output_queue_.empty() ) {
        fd.write( output_queue_.front() );
        output_queue_.pop();
    }
}

unsigned int LinkQueue::wait_time( void )
{
    const auto now = timestamp();

    rationalize( now );

    if ( next_delivery_time() <= now ) {
        return 0;
    } else {
        return next_delivery_time() - now;
    }
}

bool LinkQueue::pending_output( void ) const
{
    return not output_queue_.empty();
}
