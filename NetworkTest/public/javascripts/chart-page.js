

function getCookie(name)
{
    let cookieValue = "";
    let search = name + "=";
    if(document.cookie.length > 0)
    {
        offset = document.cookie.indexOf(search);
        if (offset !== -1)
        {
            offset += search.length;
            end = document.cookie.indexOf(";", offset);
            if (end === -1) end = document.cookie.length;
            cookieValue = unescape(document.cookie.substring(offset, end))
        }
    }
    return cookieValue;
}

function jumpTo(collection) {
    document.cookie = "collection= " + collection + "; path=home-page.html";
    window.location.href="/homepage";

}

let allDataChartCtx = document.getElementById( "allDataBarChart" );

//    ctx.height = 200;
let allDataChart = new Chart( allDataChartCtx, {
    type: 'bar',
    data: {
        labels: [ ],
        datasets: [
            {
                label: "latency / ms",
                data: [ ],
                borderColor: "rgba(0, 123, 255, 0.9)",
                borderWidth: "0",
                backgroundColor: "rgba(0, 123, 255, 0.5)",
            }
        ]
    },
    options: {
        responsive: true,
        legend : {
            position: 'top',
            labels: {
            }
        },
        scales: {
            yAxes: [ {
                ticks: {
                    beginAtZero: true
                }
            } ]
        },
        display: true
    }
} );


let staticsDataChartCtx = document.getElementById( "staticsDataBarChart" );

//    ctx.height = 200;
let staticsDataChart = new Chart( staticsDataChartCtx, {
    type: 'bar',
    data: {
        labels: [ ],
        datasets: [
            {
                label: "latency / n",
                data: [ ],
                borderColor: "rgba(0, 123, 255, 0.9)",
                borderWidth: "0",
                backgroundColor: "rgba(0, 123, 255, 0.5)",
            }
        ]
    },
    options: {
        responsive: true,
        legend : {
            position: 'top',
            labels: {
            }
        },
        scales: {
            yAxes: [ {
                ticks: {
                    beginAtZero: true
                }
            } ]
        },
        display: true
    }
} );




window.onload = function () {
    let dataId = getCookie('dataId');
    let collection = getCookie('collection');
    setDashBoard(collection, dataId);

    jQuery.post("/testdata/testdata",
        {
            collection:collection,
            dataId:dataId,
        },
        function (data) {
            data = data[0];
            setHeaderLabel(data);
            addAllDataChartData(allDataChart, data);
            addStaticsChartData(staticsDataChart, data);
        }
    );
};

function setHeaderLabel(data) {

    document.getElementById('upstreamLoss-header').innerHTML ='upstreamLoss: ' + data['upstreamLoss'] + '%';
    document.getElementById('upstreamLatency-header').innerHTML ='upstreamLatency: ' +  data['upstreamLatency'] + 'ms';
    document.getElementById('upstreamDeviation-header').innerHTML ='upstreamDeviation:' + data['upstreamDeviation'] + 'ms';

    document.getElementById('downstreamLoss-header').innerHTML ='downstreamLoss: ' + data['downstreamLoss'] + '%';
    document.getElementById('downstreamLatency-header').innerHTML ='downstreamLatency:' + data['downstreamLatency'] + 'ms';
    document.getElementById('downstreamDeviation-header').innerHTML ='downstreamDeviation: ' + data['downstreamDeviation'] + 'ms';

    document.getElementById('averageRtt-header').innerHTML ='averageRtt: ' + data['averageRtt'] + 'ms';
    document.getElementById('maxRtt-header').innerHTML ='maxRtt: ' + data['maxRtt'] + 'ms';
    document.getElementById('sendSlap-header').innerHTML ='sendSlap: ' + data['sendSlap'] + 'ms';

    document.getElementById('needSendData-header').innerHTML ='needSendData: ' + data['needSendData'] + 'bytes';
    document.getElementById('totalSendData-header').innerHTML ='totalSendData: ' + data['totalSendData'] + 'bytes';
    document.getElementById('totalReceiveData-header').innerHTML ='totalReceiveData: ' + data['totalReceiveData'] + 'bytes';

    document.getElementById('totalNumber-header').innerHTML ='totalNumber: ' + data['totalNumber'];
    document.getElementById('totalReceive-header').innerHTML ='totalReceive: ' + data['totalReceive'];


}

function addStaticsChartData(chart, testData) {
    let latencyList = testData['rttData'];
    let maxRtt = parseInt(testData['maxRtt']);
    let nLabel = parseInt(maxRtt / 20 + 1);
    let dataSet = [];
    for(let i = 0; i < latencyList.length; i++) {
        let num = parseInt(latencyList[i]);
        let labelIndex = parseInt(num / 20);
        if(dataSet[labelIndex] !== undefined)
            dataSet[labelIndex]++;
        else dataSet[labelIndex] = 1;
    }

    for(let i = 0; i < nLabel; i++) {
        chart.data.labels.push(i * 20 + 'ms');
        chart.data.datasets.forEach((dataset) => {
            dataset.data.push(parseInt(dataSet[i]));
        });
    }
    chart.update();
}

function generateAllDataChartLabels(num) {
    let length = 21;
    let result = '';
    for(let i = 0; i < length - num.toString().length; i++) {
        result += ' ';
    }
    result += num;
    return result;
}

function addAllDataChartData(chart, testData) {

    let latencyList = testData['rttData'];

    for(let a = 0; a < latencyList.length; a++) {
        let label = generateAllDataChartLabels(a);
        chart.data.labels.push(label);
        chart.data.datasets.forEach((dataset) => {
            dataset.data.push(parseInt(latencyList[a]));
        });
    }

    chart.update();
}


function setDashBoard(collection, dataId) {
    let result;
    if(collection === 'mrtpredundancy') {
        result = 'MRtp Redundancy Test Data';
    }
    else if(collection === 'mrtpreliable') {
        result = 'MRtp Reliable Test Data';
    }
    else if(collection === 'mrtpredundancynoack') {
        result = 'MRtp Redundancy No Ack Test Data';
    }
    else if(collection === 'mrtpunsequenced') {
        result = 'MRtp Unsequenced Test Data';
    }
    else if(collection === 'tcp') {
        result = 'TCP Test Data';
    }
    else if(collection === 'kcp') {
        result = 'KCP Test Data';
    }
    else if(collection === 'enet') {
        result = 'ENet Test Data';
    }
    document.getElementById('head-dash-board').innerText = result + ' of ID: ' + dataId;
}


