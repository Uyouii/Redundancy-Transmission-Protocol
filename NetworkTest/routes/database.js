const mongoose = require('mongoose');
mongoose.connect('mongodb://localhost:27017/NetworkLibraryTest');
const db = mongoose.connection;
db.on('error', console.error.bind(console, 'connection error: '));
db.on('open', function () {
    console.log('mongodb connect successfully!');
});

const mrtpRedudancySchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        packetStyle : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"mrtp_redundancy_test"
    }
);


const mrtpReliableSchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"mrtp_reliable_test"
    }
);

const mrtpUnsequencedSchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"mrtp_unsequenced_test"
    }
);

const mrtpRedundancyNoackSchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"mrtp_redundancy_noack_test"
    }
);

const kcpSchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"kcp_test"
    }
);

const tcpSchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"tcp_test"
    }
);

const enetSchema = new mongoose.Schema(
    {
        packetLength:{type:String},
        maxRtt:{type:String},
        totalReceiveUdpPacket : {type:String},
        downstreamLatency : {type:String},
        downstreamLoss  : {type:String},
        upstreamLoss : {type:String},
        time : {type:String},
        needSendData : {type:String},
        library : {type:String},
        totalReceiveData : {type:String},
        upstreamLatency :{type:String},
        rttData : {type:Object},
        totalNumber  : {type:String},
        timeStamp : {type:String},
        totalRtt : {type:String},
        totalReceive : {type:String},
        totalSendUdpPacket  : {type:String},
        totalSendData  :  {type:String},
        averageRtt : {type:String},
        upstreamDeviation: {type: String},
        downstreamDeviation: {type:String},
        sendSlap : {type:String},
    },
    {
        collection:"enet_test"
    }
);

const mrtp_redundancy_test_model = mongoose.model('mrtp_redundancy_test', mrtpRedudancySchema);
const mrtp_reliable_test_model = mongoose.model('mrtp_reliable_test', mrtpReliableSchema);
const mrtp_unsequenced_test_model = mongoose.model('mrtp_unsequenced_test', mrtpUnsequencedSchema);
const mrtp_redundancy_noack_test_model = mongoose.model('mrtp_redundancy_noack_test', mrtpRedundancyNoackSchema);
const kcp_model = mongoose.model('kcp_test', kcpSchema);
const tcp_model = mongoose.model('tcp_test', tcpSchema);
const enet_model = mongoose.model('enet_test', enetSchema);


const getMRtpRedundancyData = async() => {
    return mrtp_redundancy_test_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getMRtpReliableData = async() => {
    return mrtp_reliable_test_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getMRtpUnsequencedData = async() => {
    return mrtp_unsequenced_test_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getMRtpRedundancyNoackData = async() => {
    return mrtp_redundancy_noack_test_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getKcpData = async() => {
    return kcp_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getTcpData = async() => {
    return tcp_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getENetData = async() => {
    return enet_model.find({}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getMRtpRedundancyDataById = async(dataId) => {
    return mrtp_redundancy_test_model.find({_id:dataId}, async(err, docs) => {
        if(err) {
            console.log(err);
        }
    })
};

const getMRtpReliableDataById = async(dataId) => {
    return mrtp_reliable_test_model.find({_id:dataId}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getMRtpUnsequencedDataById = async(dataId) => {
    return mrtp_unsequenced_test_model.find({_id:dataId}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getMRtpRedundancyNoackDataById = async(dataId) => {
    return mrtp_redundancy_noack_test_model.find({_id:dataId}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getKcpDataById = async(dataId) => {
    return kcp_model.find({_id:dataId}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getTcpDataById = async(dataId) => {
    return tcp_model.find({_id:dataId}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};

const getENetDataById = async(dataId) => {
    return enet_model.find({_id:dataId}, async(err,docs) => {
        if(err) {
            console.log(err);
        }
    });
};


module.exports = {
    'getMRtpRedundancyData': getMRtpRedundancyData,
    'getMRtpReliableData': getMRtpReliableData,
    'getMRtpUnsequencedData': getMRtpUnsequencedData,
    'getMRtpRedundancyNoackData': getMRtpRedundancyNoackData,
    'getKcpData': getKcpData,
    'getTcpData': getTcpData,
    'getENetData': getENetData,
    'getMRtpRedundancyDataById': getMRtpRedundancyDataById,
    'getMRtpReliableDataById': getMRtpReliableDataById,
    'getMRtpUnsequencedDataById': getMRtpUnsequencedDataById,
    'getMRtpRedundancyNoackDataById': getMRtpRedundancyNoackDataById,
    'getKcpDataById': getKcpDataById,
    'getTcpDataById': getTcpDataById,
    'getENetDataById': getENetDataById,
};