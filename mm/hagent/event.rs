use crate::helper::*;

#[derive(Default, Debug, Clone, Copy)]
pub struct Sample {
    pub id: u64,
    pub va: u64,
    pub lat: u64,
    pub pa: u64,
}

impl From<&SampleData> for Sample {
    fn from(value: &SampleData) -> Self {
        Self {
            id: value.id,
            va: value.addr,
            lat: value.weight,
            pa: value.phys_addr,
        }
    }
}
