const router = require('koa-router')();
const db = require('./database');

router.prefix('/testdata');

router.post('/collectionlist', async (ctx, next) => {
    const collection = ctx.request.body.collection || '';
    console.log('collcetion: ' + collection);
    let testData;
    if(collection === 'mrtpredundancy') {
        testData = await db.getMRtpRedundancyData();
    }
    else if(collection === 'mrtpreliable') {
        testData = await  db.getMRtpReliableData();
    }
    else if(collection === 'mrtpredundancynoack') {
        testData = await db.getMRtpRedundancyNoackData();
    }
    else if(collection === 'mrtpunsequenced') {
        testData = await db.getMRtpUnsequencedData();
    }
    else if(collection === 'tcp') {
        testData = await db.getTcpData();
    }
    else if(collection === 'kcp') {
        testData = await db.getKcpData();
    }
    else if(collection === 'enet') {
        testData = await db.getENetData();
    }

    let result = [];
    for(let data of testData) {
        let returnData = {
            '_id': data['_id'],
            'library': data['library'],
            'upstreamLoss': data['upstreamLoss'],
            'upstreamLatency': data['upstreamLatency'],
            'upstreamDeviation': data['upstreamDeviation'],
            'downstreamLoss': data['downstreamLoss'],
            'downstreamLatency': data['downstreamLatency'],
            'downstreamDeviation': data['downstreamDeviation'],
            'sendSlap': data['sendSlap'],
            'averageRtt': data['averageRtt'],
        };
        result.push(returnData);
    }
    console.log(result);
    ctx.body = result;
});

router.post('/testdata', async (ctx, next) => {
    const dataId = ctx.request.body.dataId || '';
    const collection = ctx.request.body.collection || '';
    let testData;
    if(collection === 'mrtpredundancy') {
        testData = await db.getMRtpRedundancyDataById(dataId);
    }
    else if(collection === 'mrtpreliable') {
        testData = await  db.getMRtpReliableDataById(dataId);
    }
    else if(collection === 'mrtpredundancynoack') {
        testData = await db.getMRtpRedundancyNoackDataById(dataId);
    }
    else if(collection === 'mrtpunsequenced') {
        testData = await db.getMRtpUnsequencedDataById(dataId);
    }
    else if(collection === 'tcp') {
        testData = await db.getTcpDataById(dataId);
    }
    else if(collection === 'kcp') {
        testData = await db.getKcpDataById(dataId);
    }
    else if(collection === 'enet') {
        testData = await db.getENetDataById(dataId);
    }
    console.log(testData);
    ctx.body = testData;
});

router.get('/mrtpredundancylist', async (ctx, next) => {
    console.log("get mrtp redundancy list");
    let redundancyData = await db.getMRtpRedundancyData();
    let result = [];
    for(let data of redundancyData) {
        let returnData = {
            '_id': data['_id'],
            'library': data['library'],
            'upstreamLoss': data['upstreamLoss'],
            'upstreamLatency': data['upstreamLatency'],
            'downstreamLoss': data['downstreamLoss'],
            'downstreamLatency': data['downstreamLatency']
        };
        result.push(returnData);
    }
    console.log(result);
    ctx.body = result;
});

module.exports = router;