#include <iostream>
#include <fstream>
#include <string>
#include <getopt.h>
#include <thread>
#include <mutex>

#include "parsec/parsec_config.h"
#include "parsec/profiling.h"
#include "parsec/parsec_binary_profile.h"
#include "../dbpreader.h"

#include "sdk/perfetto.h"

#undef USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND
#define USE_HEAP_BUFFERED_PERFETTO_BACKEND

static const size_t MAX_PROTOBUF_MSG_LEN = 64*1024*1024;

const dbp_multifile_reader_t *dbp = nullptr;

class ThreadContext {
    public:
    ThreadContext() {
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
        writer = new protozero::ScatteredStreamWriter(&shb);
        shb.set_writer(writer);
        trace.Reset(writer);
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
        trace.Reset();
#endif
    }

    ~ThreadContext() {
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
        delete writer;
#endif
    }

    size_t Commit() {
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
        return trace.Finalize();
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
        return trace.get()->Finalize();
#endif
    }

    bool IsCommitted() {
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
        return trace.is_finalized();
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
        return trace.get()->is_finalized();
#endif
    }

    void Reset() {
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
        shb.Reset();
        // There must be a better way to reset the scattered stream writer, but I couldn't find it...
        delete writer;
        writer = new protozero::ScatteredStreamWriter(&shb);
        shb.set_writer(writer);
        trace.Reset(writer);
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
        trace.Reset();
#endif
    }

    auto AddPacket() {
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
        return trace.add_packet<perfetto::protos::pbzero::TracePacket>();
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
        return trace.get()->add_packet<perfetto::protos::pbzero::TracePacket>();
#endif
    }

    void AppendProcessDescriptor(const std::string &name, int32_t pid, uint64_t uuid) {
        auto pkt = AddPacket();
        perfetto::protos::pbzero::TrackDescriptor *td = pkt->set_track_descriptor();
        td->set_uuid(uuid);
        td->set_name(name);
        td->set_disallow_merging_with_system_tracks(true);
        perfetto::protos::pbzero::ProcessDescriptor *pd = td->set_process();
        pd->set_pid(pid);
        pd->set_process_name(name);
    }

    void AppendThreadDescriptor(const std::string &name, int32_t pid, int32_t tid, uint64_t parent_uuid, uint64_t uuid)
    {
        auto pkt = AddPacket();
        perfetto::protos::pbzero::TrackDescriptor *td = pkt->set_track_descriptor();
        td->set_uuid(uuid);
        td->set_parent_uuid(parent_uuid);
        td->set_disallow_merging_with_system_tracks(true);
        td->set_name(name);
        perfetto::protos::pbzero::ThreadDescriptor *th = td->set_thread();
        th->set_pid(pid);
        th->set_tid(tid);
        th->set_thread_name(name);
    }

    void AppendSliceBegin(uint64_t timestamp, const std::string &name, uint64_t track_uuid, uint32_t tps_id)
    {
        auto pkt = AddPacket();
        pkt->set_timestamp(timestamp);
        pkt->set_trusted_packet_sequence_id(tps_id);
        perfetto::protos::pbzero::TrackEvent *te = pkt->set_track_event();
        te->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN);
        te->set_track_uuid(track_uuid);
        te->set_name(name);
    }

    void AppendSliceEnd(uint64_t timestamp, uint64_t track_uuid, uint32_t tps_id)
    {
        auto pkt = AddPacket();
        pkt->set_timestamp(timestamp);
        pkt->set_trusted_packet_sequence_id(tps_id);
        perfetto::protos::pbzero::TrackEvent *te = pkt->set_track_event();
        te->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_END);
        te->set_track_uuid(track_uuid);
    }

    void AppendInstant(uint64_t timestamp,
                       uint64_t track_uuid,
                       uint32_t tps_id)
    {
        auto pkt = AddPacket();
        pkt->set_timestamp(timestamp);
        pkt->set_trusted_packet_sequence_id(tps_id);
        perfetto::protos::pbzero::TrackEvent *te = pkt->set_track_event();
        te->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_INSTANT);
        te->set_track_uuid(track_uuid);
    }

#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
    protozero::RootMessage<perfetto::protos::pbzero::Trace> trace;
    protozero::ScatteredHeapBuffer shb;
    protozero::ScatteredStreamWriter *writer;
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
    protozero::HeapBuffered<perfetto::protos::pbzero::Trace> trace;
#else
#error "Must chose between one of USE_HEAP_BUFFERED_PERFETTO_BACKEND or USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND"
#endif
};

class ParallelWriter {
    public:
        ParallelWriter(std::string &filename) :
            output(filename, std::ios::out | std::ios::trunc | std::ios::binary),
            tot(0)
        {
        }
        ~ParallelWriter() {
            std::cerr << "Closing output file" << std::endl;
            output.close();
        }

        void Flush(ThreadContext &ctx)
        {
            std::cerr << "DEBUG: new output to file" << std::endl;
            protozero::ConstBytes packet_data;
            std::size_t size = ctx.Commit();
            tot += size;
            assert(ctx.IsCommitted());
#if defined(USE_SCATTERED_HEAP_BUFFER_PERFETTO_BACKEND)
            std::cerr << "DEBUG: committed " << size << " bytes (" << tot << " bytes total)" << std::endl;
            const auto& slices = ctx.shb.GetSlices();
            if (slices.size() == 1) {
                // Fast path: the current packet fits into a single slice.
                auto slice_range = slices.begin()->GetUsedRange();
                packet_data = protozero::ConstBytes{
                  slice_range.begin,
                  static_cast<size_t>(slice_range.end - slice_range.begin)};
            } else {
                // Fallback: stitch together multiple slices.
                auto stitched_data = ctx.shb.StitchSlices();
                packet_data = protozero::ConstBytes{stitched_data.data(), stitched_data.size()};
            }
#elif defined(USE_HEAP_BUFFERED_PERFETTO_BACKEND)
            std::vector<uint8_t> buf = ctx.trace.SerializeAsArray();
            packet_data.data = buf.data();
            packet_data.size = buf.size();
            std::cerr << "DEBUG: committed " << size << " bytes, serialized " << packet_data.size << " bytes (" << tot << " bytes total)" << std::endl;
#endif
            {   // mutual exclusion block: only one thread writes in the file at the time
                const std::lock_guard<std::mutex> lock(o_mutex);
                std::cerr << "DEBUG: outputting " << packet_data.size << " bytes " << std::endl;
                output.write(reinterpret_cast<const char*>(packet_data.data), packet_data.size);
            }
            ctx.Reset();
        }

        std::mutex o_mutex;
        std::fstream output;
        size_t tot;
};

class StreamJob {
    public:
        StreamJob(const dbp_multifile_reader_t *_dbp,
                  const dbp_file_t *_file,
                  const dbp_thread_t *_thread,
                  uint64_t _process_uuid,
                  uint64_t _thread_uuid,
                  uint32_t _tps_id) :
                  dbp(_dbp), file(_file), thread(_thread),
                  process_uuid(_process_uuid), thread_uuid(_thread_uuid), tps_id(_tps_id) {}
        ~StreamJob() {}

        const dbp_multifile_reader_t *dbp;
        const dbp_file_t *file;
        const dbp_thread_t *thread;
        uint64_t process_uuid;
        uint64_t thread_uuid;
        uint32_t tps_id;
};

void convert_one_stream(std::shared_ptr<ParallelWriter> pw, std::vector<StreamJob*>jobs)
{
    StreamJob *job = nullptr;
    ThreadContext ctx;

    uint32_t how_many_jobs = INT32_MAX;

    while( !jobs.empty() ) {
        job = jobs.back();
        jobs.pop_back();
        int32_t tps_id = 1;

        auto it = dbp_iterator_new_from_thread( job->thread );
        const dbp_event_t *e = nullptr;
        uint32_t how_many_per_job = INT32_MAX;
        while( (e = dbp_iterator_current(it)) != NULL ) {
            if( KEY_IS_START( dbp_event_get_key(e) ) ) {
                auto m = dbp_iterator_find_matching_event_all_threads(it);
                if( NULL == m ) {
                    std::cerr << "   Event of class " << dbp_dictionary_name(dbp_file_get_dictionary(job->file, BASE_KEY(dbp_event_get_key(e)))) << " id " << dbp_event_get_taskpool_id(e) << ":" << dbp_event_get_event_id(e) << " at " << dbp_event_get_timestamp(e) << " does not have a match anywhere" << std::endl;
                } else {
                    auto g = dbp_iterator_current(m);

                    auto start = dbp_event_get_timestamp( e );
                    auto end = dbp_event_get_timestamp( g );

                    auto k = dbp_event_get_key(e);
                    std::string ks;
                    if( BASE_KEY(k) > 0 && BASE_KEY(k) < dbp_file_nb_dictionary_entries(job->file) ) {
                        auto dic =  dbp_file_get_dictionary(job->file, BASE_KEY(k));
                        ks = dbp_dictionary_name(dic);
                    } else {
                        ks = "Undefined Entry";
                    }

                    std::cerr << "DEBUG: appending slice begin at " << start << " on " << job->thread_uuid << " tps_id " << tps_id << " for task " << ks << std::endl;
                    ctx.AppendSliceBegin(start, ks, job->thread_uuid, tps_id);
                    if( dbp_event_get_flags( e ) & PARSEC_PROFILING_EVENT_HAS_INFO ) {
                    // TODO dump_info(tracefile, e, file);
                    }

                    std::cerr << "DEBUG: appending slice end on " << end << " on " << job->thread_uuid << " tps_id " << tps_id << " for task " << ks << std::endl;
                    ctx.AppendSliceEnd(end, job->thread_uuid, tps_id);
                    if( dbp_event_get_flags( g ) & PARSEC_PROFILING_EVENT_HAS_INFO ) {
                    // TODO dump_info(tracefile, g, file);
                    }

                    tps_id++;

                    if(ctx.Commit() >= MAX_PROTOBUF_MSG_LEN) {
                        pw->Flush(ctx);
                    }
                    if(0 == --how_many_per_job)
                        break;
                }
                dbp_iterator_delete(m);
            }
            dbp_iterator_next(it);
            if(0 == how_many_per_job)
                break;
        }
        if(nullptr != it) {
            dbp_iterator_delete(it);
        }
        delete job;
        if(0 == --how_many_jobs)
            break;
    }
    if(ctx.Commit() > 0) {
        pw->Flush(ctx);
    }
}

static int32_t make_pid(int32_t rank)
{
    return 1942+rank;
}

static int32_t make_tid(int32_t tid)
{
    return 1789+tid;
}

static int fake_trace(std::string output)
{
    std::shared_ptr<ParallelWriter> pw = std::make_shared<ParallelWriter>(output);
    std::vector<StreamJob*> jobs;
    ThreadContext ctx;
    uint64_t next_uuid = 1;
    uint64_t next_tps_id = 1;

    for(int ifd = 0; ifd < 2; ifd++) {
        uint64_t process_uuid = next_uuid++;
        int32_t pid = make_pid(ifd);
        std::string pname = std::string("fake execution - MPI rank ") + std::to_string(ifd);
        std::cerr << "Creating process " << pname << " with pid " << pid << " and uuid " << process_uuid << std::endl;
        ctx.AppendProcessDescriptor(pname, pid, process_uuid);
        for (int t = 0; t < 2; t++) {
            uint64_t thread_uuid = next_uuid++;
            uint32_t stream_tps_id = next_tps_id++;
            int32_t tid = make_tid(t);
            std::string tname = std::string("Thread ") + std::to_string(t) + " of process " + pname;
            std::cerr << "Creating thread " << tname << " with pid " << pid << " tid " << tid << " parent uuid " << process_uuid << " and thread_uuid " << thread_uuid << std::endl;
            ctx.AppendThreadDescriptor(tname, pid, tid, process_uuid, thread_uuid);
            StreamJob *job = new StreamJob(dbp, nullptr, nullptr, process_uuid, thread_uuid, stream_tps_id);
            jobs.push_back(job);
        }
    }

    pw->Flush(ctx);
    for(auto &j: jobs) {
        int32_t tps_id = 1;
        for(uint64_t t = 0; t < 2; t++) {
            uint64_t timestamp = (t + j->thread_uuid)*50;
            std::string taskname = std::string("T_") + std::to_string(t) + "_" + std::to_string(j->thread_uuid);
            std::cerr << "DEBUG: adding slice " << timestamp << " - " << (timestamp + 75) << " named " << taskname << " to  thread " << j->thread_uuid << ", tps_id " << tps_id << std::endl;
            ctx.AppendSliceBegin(timestamp, taskname, j->thread_uuid, tps_id);
            ctx.AppendSliceEnd(timestamp+75, j->thread_uuid, tps_id);
            tps_id++;
        }
        pw->Flush(ctx);
    }

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    bool fake_run = false;
    option longopts[] = {
        {"output", required_argument, NULL, 'o'},
        {"fake-run", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };
    std::string output;

    while(1) {
        const int opt = getopt_long(argc, argv, "o:fh", longopts, 0);

        if(opt == -1) {
            break;
        }

        switch(opt) {
        case 'o':
            output = optarg;
            break;
        case 'f':
            fake_run = true;
            break;
        case 'h':
            std::cerr << "Usage : " << argv[0] << " [-f] -o <output file> <input files>" << std::endl
                      << "  Where -o sets the name of the output file" << std::endl
                      << "        -f does a fake run to do internal checks" << std::endl;
            break;
        }
    }

    if(fake_run) {
        return fake_trace(output);
    }

    int nbfiles = argc - optind;
    if(nbfiles <= 0) {
        std::cerr << "See " << argv[0] << " -h -- No input file to process" << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<char *>filenames;
    filenames.reserve(nbfiles);
    for(int i = optind; i <= argc; i++) {
        filenames[i-optind] = argv[i];
    }

    dbp_multifile_reader_t *dbp = dbp_reader_open_files(nbfiles, filenames.data());
    if(nullptr == dbp) {
        std::cerr << "Unable to open one of the files" << std::endl;
        return EXIT_FAILURE;
    }

    std::shared_ptr<ParallelWriter> pw = std::make_shared<ParallelWriter>(output);
    std::vector<StreamJob*>jobs;

    {   /* Scope for the sequential context */
        ThreadContext ctx;

        /* Global infos about the system */
        for(int ifd = 0; ifd < dbp_reader_nb_files(dbp); ifd++) {
            /* TODO
            dbp_file_t *file = dbp_reader_get_file(dbp, ifd);
            for(int i = 0; i < dbp_file_nb_infos(file); i++) {
                        dbp_info_get_key(dbp_file_get_info(file, i))
                        dbp_info_get_value(dbp_file_get_info(file, i))
            }
            */
        }

        /* Do we want to do something with the dictionary? */
        for(int i = 0; i < dbp_reader_nb_dictionary_entries(dbp); i++) {
            /*  TODO?
                dbp_dictionary_t *dico = dbp_reader_get_dictionary(dbp, i);
                dbp_dictionary_name(dico);
                dbp_dictionary_attributes(dico);
            */
        }

        /* First, we dump the structure of the run sequentially, and
        * store all threads that will need to be handled in parallel
        */
        int64_t next_uuid=1;
        uint32_t next_tps_id=1;
        for(int ifd = 0; ifd < dbp_reader_nb_files(dbp); ifd++) {
            dbp_file_t *file = dbp_reader_get_file(dbp, ifd);
            uint64_t process_uuid = next_uuid++;
            int32_t pid = make_pid(dbp_file_get_rank(file));
            std::string pname = std::string(dbp_file_hr_id(file)) + " - MPI rank " + std::to_string(dbp_file_get_rank(file));
            std::cerr << "Creating process " << pname << " with pid " << pid << " and uuid " << process_uuid << std::endl;
            ctx.AppendProcessDescriptor(pname, pid, process_uuid);
            for (int t = 0; t < dbp_file_nb_threads(file); t++) {
                const dbp_thread_t *th = dbp_file_get_thread(file, t);
                uint64_t thread_uuid = next_uuid++;
                uint32_t stream_tps_id = next_tps_id++;
                int32_t tid = make_tid(t);
                std::cerr << "Creating thread " << dbp_thread_get_hr_id(th) << " with pid " << pid << " tid " << tid << " parent uuid " << process_uuid << " and thread_uuid " << thread_uuid << std::endl;
                ctx.AppendThreadDescriptor(dbp_thread_get_hr_id(th), pid, tid, process_uuid, thread_uuid);
                StreamJob *job = new StreamJob(dbp, file, th,
                                               process_uuid, thread_uuid, stream_tps_id);
                jobs.push_back(job);
            }
        }

        pw->Flush(ctx);
    }

    if(!fake_run) {
        std::thread t1(convert_one_stream, pw, jobs);
        t1.join();
    }

    dbp_reader_close_files(dbp);

    return EXIT_SUCCESS;
}