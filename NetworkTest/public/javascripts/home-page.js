

(function ($) {

    $('#test-data-table').DataTable({
        dom: 'lBfrtip',
        lengthMenu: [[10, 25, 50, -1], [10, 25, 50, "All"]],
        buttons: [
            'copy', 'csv', 'excel', 'pdf', 'print'
        ]
    });

    $('#row-select').DataTable( {
        initComplete: function () {
            this.api().columns().every( function () {
                let column = this;
                let select = $('<select class="form-control"><option value=""></option></select>')
                    .appendTo( $(column.footer()).empty() )
                    .on( 'change', function () {
                        let val = $.fn.dataTable.util.escapeRegex(
                            $(this).val()
                        );

                        column
                            .search( val ? '^'+val+'$' : '', true, false )
                            .draw();
                    } );

                column.data().unique().sort().each( function ( d, j ) {
                    select.append( '<option value="'+d+'">'+d+'</option>' )
                } );
            } );
        }
    } );
})(jQuery);


$(document).ready(function() {
    let dataTableObj = $('#test-data-table');
    dataTableObj.DataTable();

    dataTableObj.on('click', 'tr', function () {
        let data = dataTableObj.DataTable().row( this ).data();
        jumpToChartPage(data[0]);
    } );
});


window.onload = function () {

    let collection = getCookie('collection');
    if(!collection)
        collection = 'mrtpredundancy';
    setDashBoard(collection);

    $.post("/testdata/collectionlist", {
            collection:collection,
        },
        function (data) {
            setTable(data);
        }
    );
};

function setTable(data) {

    let table = $('#test-data-table').DataTable();
    for( let line of data) {
        table.row.add([
            line['_id'],
            line['library'],
            line['upstreamLoss'] + '%',
            line['upstreamLatency'] + 'ms',
            line['upstreamDeviation'] + 'ms',
            line['downstreamLoss'] + '%',
            line['downstreamLatency'] + 'ms',
            line['downstreamDeviation'] + 'ms'
        ]).draw();
    }
}

function setCookie(name,value) {
    document.cookie = name + "="+ value;
}

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
    document.cookie = "collection= " + collection + "; path=chart-page.html";
    window.location.href="/homepage";
}

function jumpToChartPage(id) {
    document.cookie = "dataId=" + id + ";path=chart-page.html";
    window.location.href="/chartpage";
}

function setDashBoard(collection) {
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
    document.getElementById('head-dash-board').innerText = result;
}


