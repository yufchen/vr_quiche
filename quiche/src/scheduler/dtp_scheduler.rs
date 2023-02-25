use crate::scheduler::Block;
use crate::scheduler::Scheduler;

pub struct DtpScheduler {
    ddl: u64,
    size: u64,
    prio: u64,
    last_block_id: Option<u64>,
    max_prio: u64,
}

impl Default for DtpScheduler {
    fn default() -> Self {
        DtpScheduler {
            ddl: 0,
            size: 0,
            prio: 999999999, // lowest priority
            last_block_id: None,
            max_prio: 2,
        }
    }
}

impl Scheduler for DtpScheduler {
    fn new() -> Self {
        info!("Create DTP Scheduler");
        Default::default()
    }

    fn select_block(
        &mut self,
        blocks_vec: &mut Vec<Block>,
        pacing_rate: f64, rtt: f64,
        _next_packet_id: u64, current_time: u64
    ) -> u64 {
        let mut min_weight = 10000000.0;
        let mut min_weight_block_id: i32 = -1;

        let mut len: i128 = -1;
        let mut ddl = 0;
        let mut size = 0;
        let mut prio = 0;

        //last send block id
        /*
        if self.last_block_id.is_some() {
            for block in blocks_vec.iter() {
                if Some(block.block_id) == self.last_block_id {
                    len = block.remaining_size as i128;
                    ddl = block.block_deadline;
                    size = block.remaining_size;
                    prio = block.block_priority;
                    break;
                }
            }
        }
        eprintln!("dtp select_block last block: len {} ddl {} size {} prior {}", len, ddl, size, prio);
        */

        let mut block_id_vec = vec![];
        for block in blocks_vec.iter() {
            if block.remaining_size > 0 {
                block_id_vec.push(block.block_id);
            }
        }

        for i in 0..blocks_vec.len() {
            let block = &blocks_vec[i];
            if block.remaining_size > 0 {
                //dependency
                if (block.block_id != block.depend_id) && block_id_vec.contains(&block.depend_id) {
                    eprintln!("{} ms, dtp skip block {}:  depend_id {}", current_time, block.block_id, block.depend_id);
                    continue;
                }


                let tempddl = block.block_deadline;
                let passed_time = current_time - block.block_create_time;
                let one_way_delay = rtt / 2.0;
                let tempsize = block.remaining_size;

                let remaining_time: f64 = tempddl as f64 - passed_time as f64 - one_way_delay - ((tempsize as f64 / pacing_rate) * 1000.0); // Bytes / (B/s) * 1000. (ms)
                eprintln!("{} ms dtp scheduler: block_id {}, tempddl: {}, passed_time: {}, one_way_delay: {}, tempsize: {}, pacing_rate: {}, remaining_time: {}",
                          current_time, block.block_id, tempddl, passed_time, one_way_delay, tempsize, pacing_rate, remaining_time);

                if remaining_time >= 0.0 {
                    let tempprio = block.block_priority;
                    let unsent_ratio = tempsize as f64 / block.block_size as f64;
                    let remaining_time_weight = remaining_time / tempddl as f64;

                    //let weight: f64 = (1.0 * remaining_time / tempddl as f64) / (1.0 - tempprio as f64 / self.max_prio as f64);
                    let weight: f64 = (((1.0 - 0.5) * remaining_time_weight) +
                        0.5 * tempprio as f64 / self.max_prio as f64) * unsent_ratio;
                    eprintln!("{} ms, dtp consider block {}: weight {}, remain_time {}, prior {}/{}, ddl {} depend_id {}", current_time, block.block_id,  weight, remaining_time, tempprio, self.max_prio, tempddl, block.depend_id);
                    if min_weight_block_id == -1 ||
                        min_weight > weight ||
                        (min_weight == weight && block.remaining_size < blocks_vec[min_weight_block_id as usize].remaining_size)
                    {
                        min_weight_block_id = i as i32;
                        min_weight = weight;
                        ddl = block.block_deadline;
                        size = block.remaining_size;
                        prio = block.block_priority;

                    }
                }
            }
        }

        if min_weight_block_id == -1 {
            for i in 0..blocks_vec.len() {
                let block = &blocks_vec[i];
                if block.remaining_size > 0 {
                    //dependency
                    if (block.block_id != block.depend_id) && block_id_vec.contains(&block.depend_id) {
                        eprintln!("{} ms, ==-1 dtp skip block {}:  depend_id {}", current_time, block.block_id, block.depend_id);
                        continue;
                    }
                    let tempddl = block.block_deadline;
                    let passed_time = current_time as f64 - block.block_create_time as f64;
                    let one_way_delay = rtt / 2.0;
                    let tempsize = block.remaining_size;

                    let remaining_time: f64 = tempddl as f64 - passed_time as f64 - one_way_delay - ((tempsize as f64 / pacing_rate) * 1000.0); // Bytes / (B/s) * 1000. (ms)
                    eprintln!("{} ms dtp scheduler: block_id {}, tempddl: {}, passed_time: {}, one_way_delay: {}, tempsize: {}, pacing_rate: {}, remaining_time: {}",
                              current_time, block.block_id, tempddl, passed_time, one_way_delay, tempsize, pacing_rate, remaining_time);

                    if passed_time >= 0.0 {
                        let tempprio = block.block_priority;
                        let unsent_ratio = tempsize as f64 / block.block_size as f64;
                        let passed_time_weight = passed_time / tempddl as f64;

                        //let weight: f64 = (1.0 * remaining_time / tempddl as f64) / (1.0 - tempprio as f64 / self.max_prio as f64);
                        let weight: f64 = (((1.0 - 0.5) * passed_time_weight) +
                            0.5 * tempprio as f64 / self.max_prio as f64) * unsent_ratio;
                        eprintln!("{} ms, ==-1 dtp consider block {}: weight {}, remain_time {}, prior {}/{}, ddl {} depend_id {}", current_time, block.block_id,  weight, passed_time, tempprio, self.max_prio, tempddl, block.depend_id);
                        if min_weight_block_id == -1 ||
                            min_weight > weight ||
                            (min_weight == weight && block.remaining_size < blocks_vec[min_weight_block_id as usize].remaining_size)
                        {
                            min_weight_block_id = i as i32;
                            min_weight = weight;
                            ddl = block.block_deadline;
                            size = block.remaining_size;
                            prio = block.block_priority;

                        }
                    }
                }
            }
        }



        self.ddl = ddl;
        self.size = size;
        self.prio = prio;

        if min_weight_block_id != -1 {
            self.last_block_id = Some(blocks_vec[min_weight_block_id as usize].block_id) ;
            eprintln!("!= -1 {}", blocks_vec[min_weight_block_id as usize].block_id);
            return blocks_vec[min_weight_block_id as usize].block_id;
        } else {
            //self.last_block_id = Some(blocks_vec[0].block_id);
            //eprintln!("== -1 {}", blocks_vec[0].block_id);
            //return blocks_vec[0].block_id;

            //BUG?
            eprintln!("==-1 {}",  self.last_block_id.unwrap());
            return self.last_block_id.unwrap();
        }
    }

    fn should_drop_block(
        &mut self,
        block: &Block,
        _pacing_rate: f64, _rtt:f64,
        _next_packet_id: u64, current_time: u64
    ) -> bool {
        let passed_time = current_time - block.block_create_time;
        if passed_time > block.block_deadline {
            //eprintln!("{} ms dtp should_drop_block: block id {} passed time ms: {}, ddl: {}, prior{}, remaining_size {},{}", current_time, block.block_id, passed_time, block.block_deadline, block.block_priority, block.remaining_size, block.block_size);
            eprintln!("drop_block, id:{} prior:{} remaining_size:{} size:{}", block.block_id, block.block_priority, block.remaining_size, block.block_size);
        }
        return passed_time > block.block_deadline;
    }
}