/* Included only by vk_gpuprof.c under TOTK_VK_TESTS. Synthetic fenced leases,
 * real FIFO/consumer/parent aggregation/encoder/rotation/close. No Vulkan init. */
static double fixture_elapsed_ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}
int vk_gpuprof_test_compact_run(int argc, char **argv) {
    unsigned seconds = 600, fps = 0, pace = 0, loss = 0, limit_frames = 0;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        char *end; unsigned long value = strtoul(argv[i + 1], &end, 10);
        if (!*argv[i + 1] || *end || value > 360000) return 2;
        if (!strcmp(argv[i], "--seconds") && value && value <= 3600) seconds = (unsigned)value;
        else if (!strcmp(argv[i], "--fps") && (!value || (value >= 20 && value <= 60))) fps = (unsigned)value;
        else if (!strcmp(argv[i], "--pace") && value <= 1) pace = (unsigned)value;
        else if (!strcmp(argv[i], "--loss") && value <= 1) loss = (unsigned)value;
        else if (!strcmp(argv[i], "--frames") && value) limit_frames = (unsigned)value;
        else return 2;
    }
    fixture_mode = 1; vk_gpuprof_on = 1; vk_gpuprof_pipeline_on = 4;
    const char *raw = getenv("TOTK_VK_GPU_PROFILE_PIPELINES_RAW");
    pipeline_raw = raw && !strcmp(raw, "1");
    pipeline_launch_phase = 17; pipeline_random = 1;
    V.timestamp_bits = 48; V.props.limits.timestampPeriod = 1;
    V.props.limits.timestampComputeAndGraphics = 1;
    snprintf(V.props.deviceName, sizeof V.props.deviceName, "host synthetic fenced-lease fixture (no GPU)");
    game_reference_w = 1600; game_reference_h = 900;
    background_start();
    struct timespec wall_start; clock_gettime(CLOCK_MONOTONIC, &wall_start);
    double logical = 0, producer_wait_ms = 0;
    unsigned frames = 0;
    uint32_t population = VK_GPUPROF_PIPE_ORDINALS;
    while (limit_frames ? frames < limit_frames : logical < seconds - 1e-8) {
        uint64_t number = ++frames;
        unsigned logical_fps = fps ? fps : 20 + ((unsigned)logical / 20 % 5) * 10;
        uint32_t phase = (pipeline_launch_phase + (uint32_t)(number - 1) * 4) & (population - 1);
        for (unsigned part = 0; part < 2; part++) {
            pthread_mutex_lock(&background_mu);
            struct timespec wait_start, wait_end; clock_gettime(CLOCK_MONOTONIC, &wait_start);
            while (!background_nfree) pthread_cond_wait(&background_cv, &background_mu);
            clock_gettime(CLOCK_MONOTONIC, &wait_end);
            producer_wait_ms += fixture_elapsed_ms(wait_start, wait_end);
            vk_gpuprof_slot *slot = background_free[--background_nfree];
            pthread_mutex_unlock(&background_mu);
            slot->n = 1; slot->nsegment = 1; slot->nt = slot->ns = slot->dropped = 0;
            slot->npipeline = slot->pipeline_timed = slot->pipeline_unselected = 0;
            slot->discard = 0; slot->sealed = part ? number : 0; slot->sequence = ++recording_sequence;
            slot->lease = slot->sequence; slot->has_pre = 0;
            slot->domain = (vk_gpuprof_time_domain){1, 1, 0, 48, 1};
            uint64_t tick = number * UINT64_C(20000000) + part * UINT64_C(5000000);
            slot->span_ticks[0] = (vk_gpuprof_timestamp){tick, 1};
            slot->span_ticks[1] = (vk_gpuprof_timestamp){tick + UINT64_C(5000000), 1};
            slot->ticks[0] = (vk_gpuprof_timestamp){tick + 100, 1};
            slot->ticks[1] = (vk_gpuprof_timestamp){tick + 1000100, 1};
            slot->rec[0] = (gp_record){.frame = number, .instance = part + 1,
                .draws = 2048, .selected = 1, .timestamp_q1 = 1, .timestamp_count = 2,
                .key = {.w = 1600, .h = 900, .samples = 1, .shader = part + 1}};
            gp_segment *s = &slot->segment[0];
            *s = (gp_segment){.frame = number, .first = 0, .last = 1,
                .pipeline_population = population, .pipeline_phase = phase};
            unsigned optional = 0;
            for (unsigned j = 0; j < 2048; j++) {
                unsigned ordinal = part * 2048 + j;
                int selected = vk_gpuprof_pipe_selected(ordinal, phase, 4, population);
                s->pipeline_observed++; s->pipeline_out_of_range += ordinal >= population;
                s->pipeline_selected += selected;
                if (!selected && optional == 1024) { s->pipeline_unretained++; continue; }
                if (!selected) optional++;
                gp_pipeline *p = &slot->pipeline[slot->npipeline++];
                unsigned key = (ordinal + (unsigned)((number - 1) / 300 % 8) * 256 +
                    (unsigned)(number / 75 % 2) * 128) & 4095;
                /* Recurrent pool plus fresh variants exercises bounded lifetime
                   eviction, not just an immortal fixed 4096-entry dictionary. */
                uint64_t variant = key + (key % 64 ? 0 : (number - 1) / 300 * 4096);
                *p = (gp_pipeline){.frame = number, .pipeline = key + 1, .ordinal = ordinal,
                    .population = population, .selected = selected, .draws = 1 + key % 7,
                    .primitives = 2 + key % 11, .unknown_draws = key % 19 == 0};
                snprintf(p->shader[0], sizeof p->shader[0], "%016llx%016llx%016llx%016llx-vs",
                    (unsigned long long)variant, (unsigned long long)(variant * 13),
                    (unsigned long long)(variant * 31), (unsigned long long)(variant * 43));
                snprintf(p->shader[1], sizeof p->shader[1], "%016llx%016llx%016llx%016llx-fs",
                    (unsigned long long)(variant * 7), (unsigned long long)(variant * 17),
                    (unsigned long long)(variant * 37), (unsigned long long)(variant * 47));
                if (selected) {
                    p->query1 = slot->pipeline_timed++ * 2 + 1; p->timestamps = 2;
                    uint64_t duration = 10000 + key * 13;
                    slot->pipeline_ticks[p->query1 - 1] = (vk_gpuprof_timestamp){tick + j * 100, 1};
                    slot->pipeline_ticks[p->query1] = (vk_gpuprof_timestamp){tick + j * 100 + duration, 1};
                    if (loss && number % 97 == 0) slot->pipeline_ticks[p->query1].available = 0;
                    if (loss && number % 101 == 0) slot->pipeline_ticks[p->query1].ticks = tick + j * 100 - 1;
                    if (loss && number % 103 == 0) p->timestamps = 1;
                    s->pipeline_timestamps += p->timestamps;
                }
            }
            if (loss && part && number % 107 == 0) { s->pipeline_dropped++; s->pipeline_unretained--; }
            if (loss && part && number == 109) atomic_fetch_add(&background_dropped_slots, 1);
            background_enqueue(&slot, 0);
        }
        population = vk_gpuprof_pipe_population(population, 4096);
        logical += 1.0 / logical_fps;
        if (pace) {
            struct timespec target = wall_start;
            uint64_t ns = (uint64_t)(logical * 1e9);
            target.tv_sec += ns / UINT64_C(1000000000);
            target.tv_nsec += ns % UINT64_C(1000000000);
            if (target.tv_nsec >= 1000000000) { target.tv_sec++; target.tv_nsec -= 1000000000; }
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL) == EINTR) {}
        }
    }
    struct timespec close_start, close_end; clock_gettime(CLOCK_MONOTONIC, &close_start);
    vk_gpuprof_shutdown(); vk_gpuprof_shutdown();
    clock_gettime(CLOCK_MONOTONIC, &close_end);
    uint64_t windows = (frames + 299) / 300;
    printf("gpuprof-compact-fixture frames=%u logical_seconds=%.6f fps=%u pace=%u loss=%u raw=%d "
        "keys=4096 retained_optional_per_lease=1024 windows=%llu serialized_bytes=%llu bytes_per_window=%.3f "
        "worker_thread_cpu_ms=%.6f thread_cpu_ms_per_window=%.6f thread_cpu_peak_window_ms=%.6f "
        "backlog_peak=%llu aggregation_memory_bytes=%zu lease_memory_bytes=%zu parent_aggregation_memory_bound_bytes=%zu "
        "aggregation_allocations=0 output_write_calls=%llu close_flush_cpu_ms=%.6f close_wall_ms=%.6f "
        "producer_wait_ms=%.6f wall_ms=%.6f dropped_slots=%llu output_failures=%llu\n",
        frames, logical, fps, pace, loss, pipeline_raw, (unsigned long long)windows,
        (unsigned long long)output_serialized_bytes, output_serialized_bytes / (double)windows,
        worker_cpu_total_ms, worker_cpu_total_ms / windows, worker_window_cpu_peak_ms,
        (unsigned long long)background_backlog_peak, sizeof compact,
        GP_BACKLOG * (sizeof(vk_gpuprof_slot) + GP_RECORDS * sizeof(gp_pipeline) +
            2 * VK_GPUPROF_PIPE_BUDGET * GP_SEGMENTS * sizeof(vk_gpuprof_timestamp)),
        sizeof groups + GP_GROUPS * sizeof(gp_group),
        (unsigned long long)output_write_calls, compact_close_cpu_ms, fixture_elapsed_ms(close_start, close_end), producer_wait_ms,
        fixture_elapsed_ms(wall_start, close_end), (unsigned long long)atomic_load(&background_dropped_slots),
        (unsigned long long)atomic_load(&output_failures));
    return atomic_load(&output_failures) != 0;
}
