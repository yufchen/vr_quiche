use crate::scheduler::Block;
use crate::scheduler::Scheduler;

pub struct DtpScheduler {
    ddl: u64,
    size: u64,
    prio: u64,
    last_block_id: Option<u64>,
    max_prio: u64,
    alpha: f64,
    beta: f64,
}

impl Default for DtpScheduler {
    fn default() -> Self {
        DtpScheduler {
            ddl: 0,
            size: 0,
            prio: 999999999, // lowest priority
            last_block_id: None,
            max_prio: 2,
            alpha: 0.5,
            beta: 100000.0
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


        let mut block_id_vec = vec![];
        for block in blocks_vec.iter() {
            if block.remaining_size > 0 {
                block_id_vec.push(block.block_id);
            }
        }

        for i in 0..blocks_vec.len() {
            let block = &blocks_vec[i];
            if block.remaining_size > 0 {

                let tempddl = block.block_deadline;
                let passed_time = current_time - block.block_create_time;
                let one_way_delay = rtt / 2.0;
                let tempsize = block.remaining_size;

                let remaining_time: f64 = tempddl as f64
                    - passed_time as f64
                    - one_way_delay
                    - (tempsize as f64 / (pacing_rate * 1024.0)) * 1000.0; // Bytes / (KB/s) * 1000. (ms)
                // eprintln!("{} ms dtp scheduler: block_id {}, tempddl: {}, passed_time: {}, one_way_delay: {}, tempsize: {}, pacing_rate: {}, remaining_time: {}",
                //           current_time, block.block_id, tempddl, passed_time, one_way_delay, tempsize, pacing_rate, remaining_time);

                let remaining_time_weight =
                    if remaining_time > 0.0 {
                        remaining_time / tempddl as f64
                    } else {
                        1.0_f64.min(- remaining_time / tempddl as f64) + self.beta
                    };
                let tempprio = block.block_priority;
                let unsent_ratio = tempsize as f64 / block.block_size as f64;
                let weight: f64 = (((1.0 - self.alpha) * remaining_time_weight) +
                    self.alpha * tempprio as f64 / self.max_prio as f64) * unsent_ratio;
                //eprintln!("{} ms, dtp consider block {}: weight {}, remain_time {}, prior {}/{}, ddl {} depend_id {}", current_time, block.block_id,  weight, remaining_time, tempprio, self.max_prio, tempddl, block.depend_id);

                if min_weight_block_id == -1 ||
                    min_weight > weight ||
                    (
                        min_weight == weight &&
                            block.remaining_size <
                                blocks_vec[min_weight_block_id as usize].remaining_size
                    ) {
                    min_weight_block_id = i as i32;
                    min_weight = weight;
                    ddl = block.block_deadline;
                    size = block.remaining_size;
                    prio = block.block_priority;
                }


            }
        }


        self.ddl = ddl;
        self.size = size;
        self.prio = prio;

        if min_weight_block_id != -1 {
            self.last_block_id = Some(blocks_vec[min_weight_block_id as usize].block_id) ;
            //eprintln!("!= -1 {}", blocks_vec[min_weight_block_id as usize].block_id);
            return blocks_vec[min_weight_block_id as usize].block_id;
        } else {
            //BUG?
            //eprintln!("==-1 {}",  self.last_block_id.unwrap());
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
            //eprintln!("drop_block, id:{} prior:{} remaining_size:{} size:{}", block.block_id, block.block_priority, block.remaining_size, block.block_size);
        }
        return passed_time > block.block_deadline;
    }
}